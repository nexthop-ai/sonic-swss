import json

from swsscommon import swsscommon
from dvslib.dvs_common import wait_for_result, PollingConfig


class TestPortchannelLacpMode(object):
    def test_Portchannel_lacp_mode(self, dvs, testlog):
        # teamd runner "independent_mode" should follow the resolved LACP mode:
        # per-PortChannel lacp_mode, else DEVICE_METADATA default, else coupled.
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL")
        meta = swsscommon.Table(self.cdb, "DEVICE_METADATA")

        polling_config = PollingConfig(polling_interval=1, timeout=30, strict=True)

        asicdb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        asic_lagtbl = swsscommon.Table(asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_LAG")
        baseline_lags = set(asic_lagtbl.getKeys())

        def _get_independent_mode(lag):
            (exit_code, output) = dvs.runcmd("teamdctl " + lag + " config dump")
            if exit_code != 0 or not output:
                return None
            try:
                return json.loads(output)["runner"].get("independent_mode", False)
            except (ValueError, KeyError, TypeError):
                return None

        def _check_mode(lag, expected):
            def _check():
                val = _get_independent_mode(lag)
                return (val == expected, val)
            wait_for_result(
                _check, polling_config,
                failure_message="teamd independent_mode for {} expected {!r}".format(lag, expected))

        base_fvs = [("admin_status", "up"), ("mtu", "9100"), ("oper_status", "up")]

        # 1. Explicit per-PortChannel modes
        tbl.set("PortChannel0011", swsscommon.FieldValuePairs(base_fvs + [("lacp_mode", "independent")]))
        tbl.set("PortChannel0012", swsscommon.FieldValuePairs(base_fvs + [("lacp_mode", "coupled")]))
        _check_mode("PortChannel0011", True)
        _check_mode("PortChannel0012", False)

        # 2. No per-PortChannel lacp_mode -> inherit DEVICE_METADATA default (independent)
        meta.set("localhost", swsscommon.FieldValuePairs([("default_lacp_mode", "independent")]))
        tbl.set("PortChannel0013", swsscommon.FieldValuePairs(base_fvs))
        _check_mode("PortChannel0013", True)

        # 3. Per-PortChannel coupled overrides the independent global default
        tbl.set("PortChannel0014", swsscommon.FieldValuePairs(base_fvs + [("lacp_mode", "coupled")]))
        _check_mode("PortChannel0014", False)

        # 4. Changing the default restarts default-mode LAGs, leaves explicit-mode LAGs alone
        meta.set("localhost", swsscommon.FieldValuePairs([("default_lacp_mode", "coupled")]))
        _check_mode("PortChannel0013", False)
        _check_mode("PortChannel0014", False)

        created = ["PortChannel0011", "PortChannel0012", "PortChannel0013", "PortChannel0014"]
        for name in created:
            tbl._del(name)
        meta.hdel("localhost", "default_lacp_mode")

        teardown_polling = PollingConfig(polling_interval=1, timeout=180, strict=True)

        def _asic_lags_drained():
            new_lags = set(asic_lagtbl.getKeys()) - baseline_lags
            return (len(new_lags) == 0, new_lags)
        wait_for_result(
            _asic_lags_drained, teardown_polling,
            failure_message="ASIC_DB LAGs from test_Portchannel_lacp_mode were not torn down")

        statedb = swsscommon.DBConnector(6, dvs.redis_sock, 0)
        state_lagtbl = swsscommon.Table(statedb, "LAG_TABLE")

        def _state_lags_cleared():
            leftover = set(state_lagtbl.getKeys()) & set(created)
            return (len(leftover) == 0, leftover)
        wait_for_result(
            _state_lags_cleared, teardown_polling,
            failure_message="STATE_DB LAG_TABLE not cleared after test_Portchannel_lacp_mode")
