#ifndef SWSS_STPORCH_H
#define SWSS_STPORCH_H

#include <map>
#include <set>
#include "orch.h"

#define STP_INVALID_INSTANCE 0xFFFF
// Instance count published when the SAI cannot report one; stpmgrd uses the same default.
#define STP_FALLBACK_MAX_INSTANCES 255
#define APP_STP_INST_PORT_FLUSH_TABLE_NAME "STP_INST_PORT_FLUSH_TABLE"

typedef enum _stp_state
{
    STP_STATE_DISABLED				= 0,
	STP_STATE_BLOCKING				= 1,
	STP_STATE_LISTENING				= 2,
	STP_STATE_LEARNING				= 3,
	STP_STATE_FORWARDING			= 4,
	STP_STATE_INVALID               = 5
}stp_state;

typedef struct StpInstEntry
{
    sai_object_id_t stp_inst_oid;
    std::set<std::string> stp_inst_vlan_list;
} StpInstEntry;


class StpOrch : public Orch
{
public:
    StpOrch(DBConnector *db, DBConnector *stateDb, vector<string> &tableNames);
    bool stpVlanFdbFlush(string vlan_alias);
    bool updateMaxStpInstance(uint32_t max_stp_instance);
    bool removeStpPorts(Port &port);
    bool removeVlanFromStpInstance(string vlan, sai_uint16_t stp_instance);

private:
<<<<<<< HEAD
=======
    /* STATE_DB STP_TABLE key/field used to persist the set of ports currently
     * err-disabled by BPDU guard, so an orchagent restart can re-assert the
     * disable. CONFIG_DB deliberately holds no record of it. */
    static constexpr const char *BPDU_GUARD_STATE_KEY = "BPDU_GUARD";
    static constexpr const char *BPDU_GUARD_PORTS_FIELD = "err_disabled_ports";

    bool readDefaultStpId();
    void publishMaxStpInstance();

>>>>>>> 791ab704 (NOS-22828: [stporch] read the default STP instance on its own (#1855))
    unique_ptr<Table> m_stpTable;
    std::map<sai_uint16_t, sai_object_id_t> m_stpInstToOid;//Mapping from STP instance id to corresponding object id
    sai_object_id_t m_defaultStpId = SAI_NULL_OBJECT_ID;
    std::map<sai_uint16_t, StpInstEntry> m_vlanAliasToStpInstanceMap;

<<<<<<< HEAD
    sai_uint16_t m_maxStpInstance;
=======
    sai_uint16_t m_maxStpInstance = 0;
    // Instances the hardware had no room for. Each is tried again once an
    // instance has been removed, not on every pass.
    std::set<sai_uint16_t> m_stpInstancesRefused;
>>>>>>> 791ab704 (NOS-22828: [stporch] read the default STP instance on its own (#1855))

    
    void doStpTask(Consumer &consumer);
    void doStpPortStateTask(Consumer &consumer);
    void doStpFastageTask(Consumer &consumer);
    void doStpVlanIntfFlushTask(Consumer &consumer);
    void doMstInstPortFlushTask(Consumer &consumer);
    
    sai_object_id_t addStpInstance(sai_uint16_t stp_instance);
    bool removeStpInstance(sai_uint16_t stp_instance);
    bool addVlanToStpInstance(string vlan, sai_uint16_t stp_instance);
    sai_object_id_t getStpInstanceOid(sai_uint16_t stp_instance);
    
    sai_object_id_t addStpPort(Port &port, sai_uint16_t stp_instance);
    bool removeStpPort(Port &port, sai_uint16_t stp_instance);
    sai_stp_port_state_t getStpSaiState(sai_uint8_t stp_state);
    bool updateStpPortState(Port &port, sai_uint16_t stp_instance, sai_uint8_t stp_state);

    void doTask(Consumer& consumer);
};
#endif /* SWSS_STPORCH_H */