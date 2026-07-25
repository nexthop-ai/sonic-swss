#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <cstdarg>
#include <string>
#include <vector>

#include <teamdctl.h>

#include "dbconnector.h"
#include "table.h"
#include "schema.h"
#include "mock_table.h"
#include "lacp_sync.h"
#include "teamdctl_mgr.h"
#include "values_store.h"

// ---------------------------------------------------------------------------
// teamdctl C API overrides. Strong symbols here shadow libteamdctl, so no real
// teamd connection is made. The collecting_confirm setter is captured so tests
// can assert exactly what tlm_teamd wrote back to teamd.
// ---------------------------------------------------------------------------
static std::string g_last_item_path;
static std::string g_last_item_value;
static int g_set_calls = 0;
static int g_set_rc = 0;   // value teamdctl_state_item_value_set returns

static void reset_teamdctl_capture()
{
    g_last_item_path.clear();
    g_last_item_value.clear();
    g_set_calls = 0;
    g_set_rc = 0;
}

struct teamdctl *teamdctl_alloc(void)
{
    static int dummy;
    return reinterpret_cast<struct teamdctl *>(&dummy);
}

void teamdctl_free(struct teamdctl *) {}

void teamdctl_set_log_fn(struct teamdctl *,
                         void (*)(struct teamdctl *tdc, int priority,
                                  const char *file, int line, const char *fn,
                                  const char *format, va_list args)) {}

int teamdctl_connect(struct teamdctl *, const char *, const char *, const char *)
{
    return 0;
}

void teamdctl_disconnect(struct teamdctl *) {}

int teamdctl_state_item_value_set(struct teamdctl *, const char *item_path, const char *value)
{
    g_last_item_path = item_path ? item_path : "";
    g_last_item_value = value ? value : "";
    g_set_calls++;
    return g_set_rc;
}

// Defensive: no test drives get_dumps() today, but this is linked in via
// teamdctl_mgr.cpp + -lteamdctl. Override it so a future get_dumps test hits a
// canned dump instead of calling real libteamdctl on the dummy handle.
int teamdctl_state_get_raw_direct(struct teamdctl *, char **p_cfg)
{
    static char canned[] = "{}";
    *p_cfg = canned;
    return 0;
}

namespace tlm_teamd_test
{
    using namespace swss;

    // Minimal independent-mode dump: only the fields LacpSync parses.
    static const char * DUMP_INDEP_TWO =
        R"({"runner":{"independent_mode":true},"ports":{)"
        R"("Ethernet0":{"runner":{"collecting_requested":true,"collecting_request_id":7}},)"
        R"("Ethernet4":{"runner":{"collecting_requested":false,"collecting_request_id":3}}}})";

    static const char * DUMP_INDEP_ONE =
        R"({"runner":{"independent_mode":true},"ports":{)"
        R"("Ethernet0":{"runner":{"collecting_requested":true,"collecting_request_id":8}}}})";

    static const char * DUMP_COUPLED =
        R"({"runner":{"active":true},"ports":{)"
        R"("Ethernet8":{"runner":{"selected":true}}}})";

    // Builds a dump carrying every field ValuesStore requires. independent_mode
    // is "" (field omitted, models old teamd), "true", or "false"; a new teamd
    // reports the field (and the member collecting fields) even when coupled.
    static std::string full_dump(const std::string & independent_mode, bool member_has_collecting)
    {
        std::string lag_runner =
            R"("runner":{"active":true,"fallback":false,"fast_rate":true,"sys_prio":65535)";
        if (!independent_mode.empty()) lag_runner += R"(,"independent_mode":)" + independent_mode;
        lag_runner += "}";

        std::string member_runner =
            R"("runner":{"actor_lacpdu_info":{"port":1,"state":63,"system":"aa:bb:cc:dd:ee:ff"},)"
            R"("partner_lacpdu_info":{"port":1,"state":63,"system":"11:22:33:44:55:66"},)"
            R"("aggregator":{"id":10,"selected":true},"selected":true,"state":"current")";
        if (member_has_collecting) member_runner += R"(,"collecting_requested":true,"collecting_request_id":7)";
        member_runner += "}";

        return R"({"setup":{"kernel_team_mode_name":"loadbalance","pid":1234},)"
             + lag_runner
             + R"(,"team_device":{"ifinfo":{"dev_addr":"aa:bb:cc:dd:ee:ff","ifindex":100}},)"
             + R"("ports":{"Ethernet0":{"ifinfo":{"dev_addr":"aa:bb:cc:dd:ee:00","ifindex":10},)"
             + R"("link":{"up":true},"link_watches":{"list":{"link_watch_0":{"up":true}}},)"
             + member_runner + "}}}";
    }

    static bool field_value(const std::vector<FieldValueTuple> & fvs,
                            const std::string & field, std::string & out)
    {
        for (const auto & fv: fvs)
        {
            if (fvField(fv) == field)
            {
                out = fvValue(fv);
                return true;
            }
        }
        return false;
    }

    // Send one confirm event through LacpSync.
    static void send_confirm(LacpSync & lacp, const std::string & key, const std::string & op,
                             std::vector<FieldValueTuple> fields)
    {
        lacp.process_member_collecting_confirmation(KeyOpFieldsValuesTuple{key, op, fields});
    }

    struct TlmTeamdTest : public ::testing::Test
    {
        DBConnector appl_db{0, "localhost", 0, 0};
        DBConnector state_db{6, "localhost", 0, 0};
        DBConnector counters_db{2, "localhost", 0, 0};
        TeamdCtlMgr mgr;

        void SetUp() override { testing_db::reset(); reset_teamdctl_capture(); }
        void TearDown() override { testing_db::reset(); }
    };

    // --- Request path (APP_DB producer) ---

    TEST_F(TlmTeamdTest, PublishesIndependentRequestsAndSkipsCoupled)
    {
        LacpSync lacp(&appl_db, mgr);
        lacp.publish_all_members_collecting_requests_db({
            {"PortChannel0001", DUMP_INDEP_TWO},
            {"PortChannel0002", DUMP_COUPLED}});

        Table app(&appl_db, APP_LAG_MEMBER_LACP_TABLE_NAME);
        std::vector<FieldValueTuple> fvs;
        std::string v;

        // Independent members are published with their request + epoch.
        ASSERT_TRUE(app.get("PortChannel0001:Ethernet0", fvs));
        ASSERT_TRUE(field_value(fvs, LACP_FIELD_COLLECTING_REQUESTED, v));
        EXPECT_EQ(v, "true");
        ASSERT_TRUE(field_value(fvs, LACP_FIELD_COLLECTING_REQUEST_ID, v));
        EXPECT_EQ(v, "7");
        ASSERT_TRUE(app.get("PortChannel0001:Ethernet4", fvs));
        ASSERT_TRUE(field_value(fvs, LACP_FIELD_COLLECTING_REQUESTED, v));
        EXPECT_EQ(v, "false");

        // A coupled LAG publishes nothing.
        EXPECT_FALSE(app.get("PortChannel0002:Ethernet8", fvs));
    }

    TEST_F(TlmTeamdTest, PrunesStaleRowsButKeepsTransient)
    {
        mgr.add_lag("PortChannel0001");
        LacpSync lacp(&appl_db, mgr);
        Table app(&appl_db, APP_LAG_MEMBER_LACP_TABLE_NAME);
        std::vector<FieldValueTuple> fvs;
        std::string v;

        lacp.publish_all_members_collecting_requests_db({{"PortChannel0001", DUMP_INDEP_TWO}});
        ASSERT_TRUE(app.get("PortChannel0001:Ethernet4", fvs));

        // Re-publish: Ethernet0's epoch changes 7->8; Ethernet4 leaves the LAG
        // (which still reports) -> its stale row is pruned.
        lacp.publish_all_members_collecting_requests_db({{"PortChannel0001", DUMP_INDEP_ONE}});
        ASSERT_TRUE(app.get("PortChannel0001:Ethernet0", fvs));
        ASSERT_TRUE(field_value(fvs, LACP_FIELD_COLLECTING_REQUEST_ID, v));
        EXPECT_EQ(v, "8");
        EXPECT_FALSE(app.get("PortChannel0001:Ethernet4", fvs));

        // LAG still tracked but absent from the dump (transient) -> row survives.
        lacp.publish_all_members_collecting_requests_db({});
        EXPECT_TRUE(app.get("PortChannel0001:Ethernet0", fvs));

        // LAG removed from tracking -> its rows are pruned.
        mgr.remove_lag("PortChannel0001");
        lacp.publish_all_members_collecting_requests_db({});
        EXPECT_FALSE(app.get("PortChannel0001:Ethernet0", fvs));
    }

    // A LAG that flips from independent to coupled still reports this cycle, so
    // its previously published rows must be withdrawn (designed behavior).
    TEST_F(TlmTeamdTest, ModeFlipToCoupledWithdrawsRows)
    {
        mgr.add_lag("PortChannel0001");
        LacpSync lacp(&appl_db, mgr);
        Table app(&appl_db, APP_LAG_MEMBER_LACP_TABLE_NAME);
        std::vector<FieldValueTuple> fvs;

        lacp.publish_all_members_collecting_requests_db({{"PortChannel0001", DUMP_INDEP_TWO}});
        ASSERT_TRUE(app.get("PortChannel0001:Ethernet0", fvs));

        // Same LAG now dumps coupled (independent_mode=false) -> rows withdrawn.
        lacp.publish_all_members_collecting_requests_db(
            {{"PortChannel0001", R"({"runner":{"independent_mode":false},"ports":{}})"}});
        EXPECT_FALSE(app.get("PortChannel0001:Ethernet0", fvs));
        EXPECT_FALSE(app.get("PortChannel0001:Ethernet4", fvs));
    }

    // --- Confirm path (teamd writeback) ---

    TEST_F(TlmTeamdTest, ConfirmWritesCollectingConfirmToTeamd)
    {
        mgr.add_lag("PortChannel0001");
        LacpSync lacp(&appl_db, mgr);

        // confirmed=true -> echo the epoch to the member's collecting_confirm.
        send_confirm(lacp, "PortChannel0001|Ethernet0", SET_COMMAND,
                     {{LACP_FIELD_COLLECTING_CONFIRMED, "true"},
                      {LACP_FIELD_COLLECTING_REQUEST_ID, "7"}});
        EXPECT_EQ(g_set_calls, 1);
        EXPECT_EQ(g_last_item_path, "ports.Ethernet0.runner.collecting_confirm");
        EXPECT_EQ(g_last_item_value, "7");

        // confirmed=false and a row delete both withdraw the latch (0).
        send_confirm(lacp, "PortChannel0001|Ethernet0", SET_COMMAND,
                     {{LACP_FIELD_COLLECTING_CONFIRMED, "false"},
                      {LACP_FIELD_COLLECTING_REQUEST_ID, "7"}});
        EXPECT_EQ(g_last_item_value, "0");
        send_confirm(lacp, "PortChannel0001|Ethernet0", DEL_COMMAND, {});
        EXPECT_EQ(g_last_item_value, "0");
        EXPECT_EQ(g_set_calls, 3);
    }

    TEST_F(TlmTeamdTest, ConfirmSkipsIncompleteAndToleratesStale)
    {
        mgr.add_lag("PortChannel0001");
        LacpSync lacp(&appl_db, mgr);

        // confirmed=true without a request id -> no epoch to echo, nothing written.
        send_confirm(lacp, "PortChannel0001|Ethernet0", SET_COMMAND,
                     {{LACP_FIELD_COLLECTING_CONFIRMED, "true"}});
        // A SET without collecting_confirmed is not a confirm -> nothing written.
        send_confirm(lacp, "PortChannel0001|Ethernet0", SET_COMMAND,
                     {{LACP_FIELD_COLLECTING_REQUEST_ID, "7"}});
        EXPECT_EQ(g_set_calls, 0);

        // teamd rejects a stale epoch -> the write is attempted once, no crash.
        g_set_rc = -22;
        send_confirm(lacp, "PortChannel0001|Ethernet0", SET_COMMAND,
                     {{LACP_FIELD_COLLECTING_CONFIRMED, "true"},
                      {LACP_FIELD_COLLECTING_REQUEST_ID, "5"}});
        EXPECT_EQ(g_set_calls, 1);
        EXPECT_EQ(g_last_item_value, "5");
    }

    TEST_F(TlmTeamdTest, ConfirmForUnconnectedLagWritesNothing)
    {
        // The LAG was never add_lag'd, so set_member_state's has_key guard must
        // drop the confirm and write nothing to teamd.
        LacpSync lacp(&appl_db, mgr);
        send_confirm(lacp, "PortChannel0007|Ethernet28", SET_COMMAND,
                     {{LACP_FIELD_COLLECTING_CONFIRMED, "true"},
                      {LACP_FIELD_COLLECTING_REQUEST_ID, "3"}});
        EXPECT_EQ(g_set_calls, 0);
    }

    // --- STATE_DB observability export (ValuesStore independent-mode gating) ---

    TEST_F(TlmTeamdTest, StateDbGatesIndependentFieldExport)
    {
        ValuesStore store(&state_db, &counters_db);
        store.update({
            {"PortChannel0001", full_dump("true", true)},   // independent, member has request
            {"PortChannel0002", full_dump("", false)},      // old-teamd coupled (no mode field)
            {"PortChannel0003", full_dump("true", false)},  // independent, member missing request
            {"PortChannel0004", full_dump("false", true)}});// new-teamd coupled (mode=false + fields)

        Table member(&state_db, "LAG_MEMBER_TABLE");
        Table lag(&state_db, "LAG_TABLE");
        std::vector<FieldValueTuple> fvs;
        std::string v;

        // Independent LAG with full member state -> request fields exported.
        ASSERT_TRUE(member.get("PortChannel0001|Ethernet0", fvs));
        ASSERT_TRUE(field_value(fvs, "runner.collecting_requested", v));
        EXPECT_EQ(v, "true");
        ASSERT_TRUE(field_value(fvs, "runner.collecting_request_id", v));
        EXPECT_EQ(v, "7");

        // Old-teamd coupled LAG (no independent_mode) -> no independent fields.
        ASSERT_TRUE(member.get("PortChannel0002|Ethernet0", fvs));
        EXPECT_FALSE(field_value(fvs, "runner.collecting_requested", v));

        // Independent LAG whose member lacks the fields -> tolerated: the row is
        // still written without member fields, and LAG-level mode is exported.
        ASSERT_TRUE(member.get("PortChannel0003|Ethernet0", fvs));
        EXPECT_FALSE(field_value(fvs, "runner.collecting_requested", v));
        ASSERT_TRUE(lag.get("PortChannel0003", fvs));
        ASSERT_TRUE(field_value(fvs, "runner.independent_mode", v));
        EXPECT_EQ(v, "true");

        // New-teamd COUPLED LAG reports independent_mode=false AND per-member
        // collecting fields. The member fields must NOT be exported (gated on
        // mode=="true"), but the LAG-level mode is still visible as "false".
        ASSERT_TRUE(member.get("PortChannel0004|Ethernet0", fvs));
        EXPECT_FALSE(field_value(fvs, "runner.collecting_requested", v));
        EXPECT_FALSE(field_value(fvs, "runner.collecting_request_id", v));
        ASSERT_TRUE(lag.get("PortChannel0004", fvs));
        ASSERT_TRUE(field_value(fvs, "runner.independent_mode", v));
        EXPECT_EQ(v, "false");
    }

    // A truncated/unparseable dump must be treated like a dump failure (rows
    // kept), not like a LAG that reported without its members (rows withdrawn).
    TEST_F(TlmTeamdTest, MalformedDumpKeepsRows)
    {
        mgr.add_lag("PortChannel0001");
        LacpSync lacp(&appl_db, mgr);
        Table app(&appl_db, APP_LAG_MEMBER_LACP_TABLE_NAME);
        std::vector<FieldValueTuple> fvs;

        lacp.publish_all_members_collecting_requests_db({{"PortChannel0001", DUMP_INDEP_TWO}});
        ASSERT_TRUE(app.get("PortChannel0001:Ethernet0", fvs));

        lacp.publish_all_members_collecting_requests_db({{"PortChannel0001", "{ this is not json"}});
        EXPECT_TRUE(app.get("PortChannel0001:Ethernet0", fvs));
        EXPECT_TRUE(app.get("PortChannel0001:Ethernet4", fvs));
    }

    // On restart the publish cache is empty, so rows a previous incarnation left
    // in APP_DB must be seeded from the table (ctor) to be reconcilable: a live
    // member is overwritten with current state, a departed one is pruned.
    TEST_F(TlmTeamdTest, RestartReconcilesOrphanedRows)
    {
        ProducerStateTable pst(&appl_db, APP_LAG_MEMBER_LACP_TABLE_NAME);
        pst.set("PortChannel0001:Ethernet0",
                {{LACP_FIELD_COLLECTING_REQUESTED, "true"}, {LACP_FIELD_COLLECTING_REQUEST_ID, "1"}});
        pst.set("PortChannel0009:Ethernet40",
                {{LACP_FIELD_COLLECTING_REQUESTED, "true"}, {LACP_FIELD_COLLECTING_REQUEST_ID, "2"}});

        // Fresh incarnation: only PortChannel0001 is still tracked.
        mgr.add_lag("PortChannel0001");
        LacpSync lacp(&appl_db, mgr);
        lacp.publish_all_members_collecting_requests_db({{"PortChannel0001", DUMP_INDEP_ONE}});

        Table app(&appl_db, APP_LAG_MEMBER_LACP_TABLE_NAME);
        std::vector<FieldValueTuple> fvs;
        std::string v;

        // Live member reconciled to the current epoch (8, from DUMP_INDEP_ONE).
        ASSERT_TRUE(app.get("PortChannel0001:Ethernet0", fvs));
        ASSERT_TRUE(field_value(fvs, LACP_FIELD_COLLECTING_REQUEST_ID, v));
        EXPECT_EQ(v, "8");

        // Orphaned row for a LAG that no longer exists is pruned (not leaked).
        EXPECT_FALSE(app.get("PortChannel0009:Ethernet40", fvs));
    }

    // Pre-orchagent an orphan lives only in the ProducerStateTable staging
    // table; the restart seed must find and prune it there too.
    TEST_F(TlmTeamdTest, RestartReconcilesStagingOrphan)
    {
        Table staging(&appl_db, std::string("_") + APP_LAG_MEMBER_LACP_TABLE_NAME);
        staging.set("PortChannel0009:Ethernet40",
                    {{LACP_FIELD_COLLECTING_REQUESTED, "true"}, {LACP_FIELD_COLLECTING_REQUEST_ID, "2"}});

        // Fresh incarnation: PortChannel0009 is no longer tracked.
        mgr.add_lag("PortChannel0001");
        LacpSync lacp(&appl_db, mgr);
        lacp.publish_all_members_collecting_requests_db({{"PortChannel0001", DUMP_INDEP_ONE}});

        std::vector<FieldValueTuple> fvs;
        EXPECT_FALSE(staging.get("PortChannel0009:Ethernet40", fvs));
    }
}
