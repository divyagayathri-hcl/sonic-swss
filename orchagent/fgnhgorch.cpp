#include <assert.h>
#include <inttypes.h>
#include "fgnhgorch.h"
#include "orch_zmq_config.h"
#include "routeorch.h"
#include "logger.h"
#include "swssnet.h"
#include "crmorch.h"
#include <array>
#include <algorithm>

#define LINK_DOWN    0
#define LINK_UP      1

extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gSwitchId;

extern sai_next_hop_group_api_t*    sai_next_hop_group_api;
extern sai_route_api_t*             sai_route_api;

extern RouteOrch *gRouteOrch;
extern CrmOrch *gCrmOrch;
extern PortsOrch *gPortsOrch;

FgNhgOrch::FgNhgOrch(DBConnector *db, DBConnector *appDb, DBConnector *stateDb, vector<table_name_with_pri_t> &tableNames, NeighOrch *neighOrch, IntfsOrch *intfsOrch, VRFOrch *vrfOrch) :
        Orch(db, tableNames),
        m_zmqClient(create_local_zmq_client(ORCH_NORTHBOND_ROUTE_ZMQ_ENABLED, false)),
        m_neighOrch(neighOrch),
        m_intfsOrch(intfsOrch),
        m_vrfOrch(vrfOrch),
        m_stateWarmRestartRouteTable(stateDb, STATE_FG_ROUTE_TABLE_NAME),
        m_routeTable(createProducerStateTable(appDb, APP_ROUTE_TABLE_NAME, m_zmqClient))
{
    SWSS_LOG_ENTER();
    isFineGrainedConfigured = false;
    gPortsOrch->attach(this);
}


void FgNhgOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();
    assert(cntx);

    switch(type) {
        case SUBJECT_TYPE_PORT_OPER_STATE_CHANGE:
        {
            PortOperStateUpdate *update = reinterpret_cast<PortOperStateUpdate *>(cntx);
            for (auto &fgNhgEntry : m_FgNhgs)
            {
                auto entry = fgNhgEntry.second.links.find(update->port.m_alias);
                if (entry != fgNhgEntry.second.links.end())
                {
                    for (auto ip : entry->second)
                    {
                        NextHopKey nhk;
                        MacAddress macAddress;
                        auto nexthop_entry = fgNhgEntry.second.next_hops.find(ip);

                        if (update->operStatus == SAI_PORT_OPER_STATUS_UP)
                        {
                            if (nexthop_entry == fgNhgEntry.second.next_hops.end())
                            {
                                SWSS_LOG_WARN("Hit unexpected condition where structs are out of sync");
                            }
                            nexthop_entry->second.link_oper_state = LINK_UP;
                            SWSS_LOG_INFO("Updated %s associated with %s to state up",
                                    update->port.m_alias.c_str(), ip.to_string().c_str());

                            if (!m_neighOrch->getNeighborEntry(ip, nhk, macAddress))
                            {
                                continue;
                            }

                            if (!validNextHopInNextHopGroup(nhk))
                            {
                                SWSS_LOG_WARN("Failed validNextHopInNextHopGroup for nh %s ip %s",
                                        nhk.to_string().c_str(), ip.to_string().c_str());
                            }
                        }
                        else if (update->operStatus == SAI_PORT_OPER_STATUS_DOWN)
                        {
                            if (nexthop_entry == fgNhgEntry.second.next_hops.end())
                            {
                                SWSS_LOG_WARN("Hit unexpected condition where structs are out of sync");
                            }
                            nexthop_entry->second.link_oper_state = LINK_DOWN;
                            SWSS_LOG_INFO("Updated %s associated with %s to state down",
                                    update->port.m_alias.c_str(), ip.to_string().c_str());

                            if (!m_neighOrch->getNeighborEntry(ip, nhk, macAddress))
                            {
                                continue;
                            }

                            if (!invalidNextHopInNextHopGroup(nhk))
                            {
                                SWSS_LOG_WARN("Failed validNextHopInNextHopGroup for nh %s ip %s",
                                        nhk.to_string().c_str(), ip.to_string().c_str());
                            }
                        }
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

bool FgNhgOrch::bake()
{
    SWSS_LOG_ENTER();

    deque<KeyOpFieldsValuesTuple> entries;
    vector<string> keys;
    m_stateWarmRestartRouteTable.getKeys(keys);

    SWSS_LOG_NOTICE("Warm reboot: recovering entry %zu from state", keys.size());

    for (const auto &key : keys)
    {
        vector<FieldValueTuple> tuples;
        m_stateWarmRestartRouteTable.get(key, tuples);

        NextHopIndexMap nhop_index_map(tuples.size(), std::string());
        for (const auto &tuple : tuples)
        {
            const auto index = stoi(fvField(tuple));
            const auto nextHop = fvValue(tuple);

            nhop_index_map[index] = nextHop;
            SWSS_LOG_INFO("Storing next hop %s at index %d", nhop_index_map[index].c_str(), index);
        }

        // Recover nexthop with index relationship
        m_recoveryMap[key] = nhop_index_map;

        m_stateWarmRestartRouteTable.del(key);
    }

    return Orch::bake();
}

/* calculateBankHashBucketStartIndices: generates the hash_bucket_indices for all banks
 * and stores it in fgNhgEntry for the group.
 * The function will identify the # of next-hops assigned to each bank and
 * assign the total number of hash buckets for a bank, based on the proportional
 * number of next-hops in the bank.
 * eg: Bank0: 6 nh, Bank1: 3 nh, total buckets: 30 =>
 *      calculateBankHashBucketStartIndices: Bank0: Bucket# 0-19, Bank1: Bucket# 20-29
 */
void FgNhgOrch::calculateBankHashBucketStartIndices(FgNhgEntry *fgNhgEntry)
{
    SWSS_LOG_ENTER();

    vector<uint32_t> memb_per_bank;
    uint32_t buckets_per_nexthop;
    uint32_t extra_buckets;
    uint32_t split_extra_buckets_among_bank;
    uint32_t num_banks = 0;
    if (fgNhgEntry->match_mode == FGMatchMode::PREFIX_BASED)
    {
        // For prefix_based match_mode there is no bank configuration, so a single bank is used
        num_banks = 1;
        memb_per_bank.push_back(fgNhgEntry->max_next_hops); // all nexthops are in bank 0
        buckets_per_nexthop = fgNhgEntry->real_bucket_size / fgNhgEntry->max_next_hops;
        extra_buckets = fgNhgEntry->real_bucket_size - (buckets_per_nexthop * fgNhgEntry->max_next_hops);
    }
    else
    {
        for (auto nh : fgNhgEntry->next_hops)
        {
            while (nh.second.bank + 1 > num_banks)
            {
                num_banks++;
                memb_per_bank.push_back(0);
            }
            memb_per_bank[nh.second.bank] = memb_per_bank[nh.second.bank] + 1;
        }

        buckets_per_nexthop = fgNhgEntry->real_bucket_size / ((uint32_t)fgNhgEntry->next_hops.size());
        extra_buckets = fgNhgEntry->real_bucket_size - (buckets_per_nexthop * ((uint32_t)fgNhgEntry->next_hops.size()));
    }
    split_extra_buckets_among_bank = extra_buckets / num_banks;
    extra_buckets = extra_buckets - (split_extra_buckets_among_bank * num_banks);

    uint32_t prev_idx = 0;

    for (uint32_t i = 0; i < memb_per_bank.size(); i++)
    {
        BankIndexRange bir;
        bir.start_index = prev_idx;
        bir.end_index = bir.start_index + (buckets_per_nexthop * memb_per_bank[i]) + split_extra_buckets_among_bank - 1;
        if (extra_buckets > 0)
        {
            bir.end_index = bir.end_index + 1;
            extra_buckets--;
        }
        if (i == fgNhgEntry->hash_bucket_indices.size())
        {
            fgNhgEntry->hash_bucket_indices.push_back(bir);
        }
        else
        {
            fgNhgEntry->hash_bucket_indices[i] = bir;
        }
        prev_idx = bir.end_index + 1;
        SWSS_LOG_INFO("Calculate_bank_hash_bucket_start_indices: bank %d, si %d, ei %d",
                       i, fgNhgEntry->hash_bucket_indices[i].start_index, fgNhgEntry->hash_bucket_indices[i].end_index);
    }
}


void FgNhgOrch::setStateDbRouteEntry(const IpPrefix &ipPrefix, uint32_t index, NextHopKey nextHop)
{
    SWSS_LOG_ENTER();

    string key = ipPrefix.to_string();
    string field = std::to_string(index);
    string value = nextHop.to_string();

    m_stateWarmRestartRouteTable.hset(key, field, value);

    SWSS_LOG_INFO("Set state db entry for ip prefix %s next hop %s with index %d",
                  key.c_str(), value.c_str(), index);
}

bool FgNhgOrch::writeHashBucketChange(FGNextHopGroupEntry *syncd_fg_route_entry, HashBucketIdx index, sai_object_id_t nh_oid,
        const IpPrefix &ipPrefix, NextHopKey nextHop)
{
    SWSS_LOG_ENTER();

    sai_attribute_t nhgm_attr;
    nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    nhgm_attr.value.oid = nh_oid;
    sai_status_t status = sai_next_hop_group_api->set_next_hop_group_member_attribute(
                                                              syncd_fg_route_entry->nhopgroup_members[index],
                                                              &nhgm_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set next hop oid %" PRIx64 " member %" PRIx64 ": %d",
            syncd_fg_route_entry->nhopgroup_members[index], nh_oid, status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_NEXT_HOP_GROUP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    setStateDbRouteEntry(ipPrefix, index, nextHop);
    return true;
}


bool FgNhgOrch::createFineGrainedNextHopGroup(FGNextHopGroupEntry &syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        const NextHopGroupKey &nextHops)
{
    SWSS_LOG_ENTER();
    string platform = getenv("platform") ? getenv("platform") : "";
    sai_attribute_t nhg_attr;
    vector<sai_attribute_t> nhg_attrs;

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    nhg_attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_FINE_GRAIN_ECMP;
    nhg_attrs.push_back(nhg_attr);

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_CONFIGURED_SIZE;
    nhg_attr.value.s32 = fgNhgEntry->configured_bucket_size;
    nhg_attrs.push_back(nhg_attr);

    sai_object_id_t next_hop_group_id;

    if (!gRouteOrch->createFineGrainedNextHopGroup(next_hop_group_id, nhg_attrs))
    {
        SWSS_LOG_ERROR("Failed to create next hop group %s",
                       nextHops.to_string().c_str());
        return false;
    }

    syncd_fg_route_entry.next_hop_group_id = next_hop_group_id;

    if (platform == VS_PLATFORM_SUBSTRING)
    {
       /* TODO: need implementation for SAI_NEXT_HOP_GROUP_ATTR_REAL_SIZE */
        fgNhgEntry->real_bucket_size = fgNhgEntry->configured_bucket_size;
    }
    else
    {
        nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_REAL_SIZE;
        nhg_attr.value.u32 = 0;
        sai_status_t status = sai_next_hop_group_api->get_next_hop_group_attribute(next_hop_group_id, 1, &nhg_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to query next hop group %s SAI_NEXT_HOP_GROUP_ATTR_REAL_SIZE, rv:%d",
                       nextHops.to_string().c_str(), status);
            task_process_status handle_status = handleSaiGetStatus(SAI_API_NEXT_HOP_GROUP, status);
            if (handle_status != task_process_status::task_success)
            {
                if (!removeFineGrainedNextHopGroup(&syncd_fg_route_entry))
                {
                    SWSS_LOG_ERROR("Failed to clean-up after next hop group real_size query failure");
                }
                return false;
            }
        }
        fgNhgEntry->real_bucket_size = nhg_attr.value.u32;
    }
    // Initialize the vector to store sai next hop group members
    syncd_fg_route_entry.nhopgroup_members.resize(fgNhgEntry->real_bucket_size, SAI_NULL_OBJECT_ID);
    calculateBankHashBucketStartIndices(fgNhgEntry);

    SWSS_LOG_NOTICE("fgnhgorch created next hop group %s of size %d", nextHops.to_string().c_str(), fgNhgEntry->real_bucket_size);
    return true;
}


bool FgNhgOrch::removeFineGrainedNextHopGroup(FGNextHopGroupEntry *syncd_fg_route_entry)
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    for (auto &nhgm : syncd_fg_route_entry->nhopgroup_members)
    {
        status = sai_next_hop_group_api->remove_next_hop_group_member(nhgm);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove next hop group member %" PRIx64 ", rv:%d",
                nhgm, status);
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEXT_HOP_GROUP, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    }

    if (!gRouteOrch->removeFineGrainedNextHopGroup(syncd_fg_route_entry->next_hop_group_id))
    {
        SWSS_LOG_ERROR("Failed to remove nhgid %" PRIx64 " return failure",
                syncd_fg_route_entry->next_hop_group_id);
        return false;
    }

    return true;
}


bool FgNhgOrch::modifyRoutesNextHopId(sai_object_id_t vrf_id, const IpPrefix &ipPrefix, sai_object_id_t next_hop_id)
{
    SWSS_LOG_ENTER();

    sai_route_entry_t route_entry;
    sai_attribute_t route_attr;

    route_entry.vr_id = vrf_id;
    route_entry.switch_id = gSwitchId;
    copy(route_entry.destination, ipPrefix);

    route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    route_attr.value.oid = next_hop_id;

    sai_status_t status = sai_route_api->set_route_entry_attribute(&route_entry, &route_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set route %s with packet action forward, %d",
                       ipPrefix.to_string().c_str(), status);
        return false;
    }

    return true;
}


bool FgNhgOrch::validNextHopInNextHopGroup(const NextHopKey& nexthop)
{
    SWSS_LOG_ENTER();

    for (auto &route_tables : m_syncdFGRouteTables)
    {
        for (auto &route_table : route_tables.second)
        {
            if (!(route_table.second.nhg_key.contains(nexthop)))
            {
                continue;
            }

            FGNextHopGroupEntry *syncd_fg_route_entry = &(route_table.second);
            FgNhgEntry *fgNhgEntry = 0;
            auto prefix_entry = m_fgNhgPrefixes.find(route_table.first);
            if (prefix_entry == m_fgNhgPrefixes.end())
            {
                auto member_entry = m_fgNhgNexthops.find(nexthop.ip_address);
                if (member_entry == m_fgNhgNexthops.end())
                {
                    SWSS_LOG_ERROR("fgNhgOrch got a validNextHopInNextHopGroup for non-configured FG ECMP entry");
                    return false;
                }
                fgNhgEntry = member_entry->second;
            }
            else
            {
                fgNhgEntry = prefix_entry->second;
            }
            std::map<NextHopKey,sai_object_id_t> nhopgroup_members_set;
            std::vector<BankMemberChanges> bank_member_changes(
                fgNhgEntry->hash_bucket_indices.size(), BankMemberChanges());

            if (syncd_fg_route_entry->active_nexthops.find(nexthop) !=
                    syncd_fg_route_entry->active_nexthops.end())
            {
                return true;
            }

            if (fgNhgEntry->hash_bucket_indices.size() == 0 && syncd_fg_route_entry->points_to_rif)
            {
                /* Only happens the 1st time when hash_bucket_indices are not inited
                 */
                if (fgNhgEntry->match_mode == FGMatchMode::PREFIX_BASED)
                {
                    // create a single bank in case of prefix_based match mode
                    bank_member_changes.push_back(BankMemberChanges());
                }
                else
                {
                    for (auto it : fgNhgEntry->next_hops)
                    {
                        while (bank_member_changes.size() <= it.second.bank)
                        {
                            bank_member_changes.push_back(BankMemberChanges());
                        }
                    }
                }
            }

            bank_member_changes[fgNhgEntry->next_hops[nexthop.ip_address].bank].
                    nhs_to_add.push_back(nexthop);
            nhopgroup_members_set[nexthop] = m_neighOrch->getNextHopId(nexthop);

            if (syncd_fg_route_entry->points_to_rif)
            {
                // RIF route is now neigh resolved: create Fine Grained ECMP
                if (!createFineGrainedNextHopGroup(*syncd_fg_route_entry, fgNhgEntry, syncd_fg_route_entry->nhg_key))
                {
                    return false;
                }

                if (!setNewNhgMembers(*syncd_fg_route_entry, fgNhgEntry, bank_member_changes, nhopgroup_members_set, route_table.first))
                {
                    return false;
                }

                if (!modifyRoutesNextHopId(route_tables.first, route_table.first, syncd_fg_route_entry->next_hop_group_id))
                {
                    return false;
                }
            }
            else
            {
                for (auto active_nh : syncd_fg_route_entry->active_nexthops)
                {
                    // include the existing active nhs into the configured bank
                    bank_member_changes[fgNhgEntry->next_hops[active_nh.ip_address].bank].
                        active_nhs.push_back(active_nh);
                }

                if (!computeAndSetHashBucketChanges(syncd_fg_route_entry, fgNhgEntry,
                        bank_member_changes, nhopgroup_members_set, route_table.first))
                {
                    SWSS_LOG_ERROR("Failed to set fine grained next hop %s",
                        nexthop.to_string().c_str());
                    return false;
                }
            }

            m_neighOrch->increaseNextHopRefCount(nexthop);

            SWSS_LOG_INFO("FG nh %s for prefix %s is up",
                    nexthop.to_string().c_str(), route_table.first.to_string().c_str());
        }
    }

    return true;
}


bool FgNhgOrch::invalidNextHopInNextHopGroup(const NextHopKey& nexthop)
{
    SWSS_LOG_ENTER();

    for (auto &route_tables : m_syncdFGRouteTables)
    {
        for (auto &route_table : route_tables.second)
        {
            if (!(route_table.second.nhg_key.contains(nexthop)))
            {
                continue;
            }

            FGNextHopGroupEntry *syncd_fg_route_entry = &(route_table.second);
            FgNhgEntry *fgNhgEntry = 0;
            auto prefix_entry = m_fgNhgPrefixes.find(route_table.first);
            if (prefix_entry == m_fgNhgPrefixes.end())
            {
                auto member_entry = m_fgNhgNexthops.find(nexthop.ip_address);
                if (member_entry == m_fgNhgNexthops.end())
                {
                    SWSS_LOG_ERROR("fgNhgOrch got an invalidNextHopInNextHopGroup for non-configured FG ECMP entry");
                    return false;
                }
                fgNhgEntry = member_entry->second;
            }
            else
            {
                fgNhgEntry = prefix_entry->second;
            }

            std::map<NextHopKey,sai_object_id_t> nhopgroup_members_set;

            std::vector<BankMemberChanges> bank_member_changes(
                fgNhgEntry->hash_bucket_indices.size(), BankMemberChanges());

            if (syncd_fg_route_entry->active_nexthops.find(nexthop) ==
                    syncd_fg_route_entry->active_nexthops.end())
            {
                return true;
            }

            for (auto active_nh : syncd_fg_route_entry->active_nexthops)
            {
                if (active_nh.ip_address == nexthop.ip_address &&
                        active_nh.alias == nexthop.alias)
                {
                    continue;
                }

                bank_member_changes[fgNhgEntry->next_hops[active_nh.ip_address].bank].
                    active_nhs.push_back(active_nh);

                nhopgroup_members_set[active_nh] = m_neighOrch->getNextHopId(active_nh);
            }

            bank_member_changes[fgNhgEntry->next_hops[nexthop.ip_address].bank].
                    nhs_to_del.push_back(nexthop);

            if (!computeAndSetHashBucketChanges(syncd_fg_route_entry, fgNhgEntry,
                    bank_member_changes, nhopgroup_members_set, route_table.first))
            {
                SWSS_LOG_ERROR("Failed to set fine grained next hop %s",
                    nexthop.to_string().c_str());
                return false;
            }

            m_neighOrch->decreaseNextHopRefCount(nexthop);

            SWSS_LOG_INFO("FG nh %s for prefix %s is down",
                    nexthop.to_string().c_str(), route_table.first.to_string().c_str());
        }
    }

    return true;
}


/* setActiveBankHashBucketChanges: Sets hash buckets for active banks and called on a PER bank basis
 * This function deals with a scenario where next-hop changes occurred for the route,
 * and the next-hop change didn't cause an entire bank to go active/inactive.
 * The function uses bank_member_changes to compute the hash buckets to modify, in order to satisfy the next-hop
 * availability for the route/neigh.
 * Eg: Prefix A had nhs 1, 2, 3 with 1, 2, 3, being equally distributed over hash buckets
 * 0-59(20 buckets per nh). If there was a nh removal of nh 2, this fn would equally redistribute hash buckets
 * for nh 2 to nh 1 and nh 3. Leading to 30 hash buckets, each, for nh 1 and nh 3, and none for nh 2.
 * Thereby achieving consistent and layered hashing.
 */
bool FgNhgOrch::setActiveBankHashBucketChanges(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        uint32_t syncd_bank, BankMemberChanges bank_member_change,
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    uint32_t add_idx = 0, del_idx = 0;
    FGNextHopGroupMap *bank_fgnhg_map = &(syncd_fg_route_entry->syncd_fgnhg_map[syncd_bank]);

    // Replace hash bucket indices of deleted NHs with added NHs
    while(del_idx < bank_member_change.nhs_to_del.size() &&
            add_idx < bank_member_change.nhs_to_add.size())
    {
        // get the hash bucket indices for the deleted NHs
        HashBuckets *hash_buckets = &(bank_fgnhg_map->at(bank_member_change.nhs_to_del[del_idx]));
        // fill the hash bucket indices with the added NHs
        for (uint32_t i = 0; i < hash_buckets->size(); i++)
        {
            if (!writeHashBucketChange(syncd_fg_route_entry, hash_buckets->at(i),
                        nhopgroup_members_set[bank_member_change.nhs_to_add[add_idx]],
                        ipPrefix, bank_member_change.nhs_to_add[add_idx]))
            {
                return false;
            }
        }

        (*bank_fgnhg_map)[bank_member_change.nhs_to_add[add_idx]] =*hash_buckets;

        bank_fgnhg_map->erase(bank_member_change.nhs_to_del[del_idx]);
        bank_member_change.active_nhs.push_back(bank_member_change.nhs_to_add[add_idx]);
        syncd_fg_route_entry->active_nexthops.erase(bank_member_change.nhs_to_del[del_idx]);
        syncd_fg_route_entry->active_nexthops.insert(bank_member_change.nhs_to_add[add_idx]);

        del_idx++;
        add_idx++;
    }

    /* Given that we resolved add + del on a bank in the above while stmt
     * We will either have add OR delete left to do, and the logic below
     * relies on this fact
     */
    if (del_idx < bank_member_change.nhs_to_del.size())
    {
        uint32_t num_buckets_in_bank = 1 + fgNhgEntry->hash_bucket_indices[syncd_bank].end_index -
            fgNhgEntry->hash_bucket_indices[syncd_bank].start_index;
        uint32_t exp_bucket_size = num_buckets_in_bank / (uint32_t)bank_member_change.active_nhs.size();
        uint32_t num_nhs_with_one_more = (num_buckets_in_bank % (uint32_t)bank_member_change.active_nhs.size());
        auto active_nh_list = bank_member_change.active_nhs;
        bool move_bkt = false;
        bool remove_nh = false;

        auto it = active_nh_list.begin();
        while (del_idx < bank_member_change.nhs_to_del.size())
        {
            HashBuckets *hash_buckets = &(bank_fgnhg_map->at(bank_member_change.nhs_to_del[del_idx]));

            uint32_t bkt_idx = 0;
            while(bkt_idx < hash_buckets->size())
            {
                move_bkt = false; remove_nh = false;
                // wraparound the active_nh_list if needed
                if (it == active_nh_list.end())
                {
                    it = active_nh_list.begin();
                    if (it == active_nh_list.end())
                    {
                        // this can neven happen
                        SWSS_LOG_ERROR("%s Unexpected no more active NHs before adding the %zu buckets, exp_bucket_size(%d)",
                                       ipPrefix.to_string().c_str(),
                                       hash_buckets->size(), exp_bucket_size);
                        return false;
                    }
                }

                /* Logic below ensure that # hash buckets assigned to a nh is equalized,
                 * we could have used simple round robin to reassign hash buckets to
                 * other available nhs, but for cases where # hash buckets is not
                 * divisible by # of nhs, simple round robin can make the hash bucket
                 * distribution non-ideal, thereby nhs can attract unequal traffic */
                if (bank_fgnhg_map->at(*it).size() == exp_bucket_size)
                {
                    if (num_nhs_with_one_more == 0)
                    {

                        SWSS_LOG_INFO("%s already reached expected bucket size %d, don't add more buckets, bkt_idx(%d)",
                                      (*it).to_string().c_str(), exp_bucket_size, bkt_idx);
                        remove_nh = true;
                    }
                    else
                    {
                        num_nhs_with_one_more--;
                        SWSS_LOG_INFO("%s reached %d, don't add more buckets after this one, num_nhs_with_one_more remaining=%d, bkt_idx(%d)",
                                      (*it).to_string().c_str(), exp_bucket_size + 1, num_nhs_with_one_more, bkt_idx);
                        move_bkt = true;
                        remove_nh = true;
                    }
                }
                else
                {
                    SWSS_LOG_INFO("%s has not reached min expected size of %d, keep adding to this nexthop, bkt_idx(%d)",
                                  (*it).to_string().c_str(), exp_bucket_size, bkt_idx);
                    move_bkt = true;
                }

                if (move_bkt)
                {
                    if (!writeHashBucketChange(syncd_fg_route_entry, hash_buckets->at(bkt_idx),
                                               nhopgroup_members_set[*it], ipPrefix, *it))
                    {
                        return false;
                    }
                    bank_fgnhg_map->at(*it).push_back(hash_buckets->at(bkt_idx));
                    bkt_idx++;
                }
                if (remove_nh)
                {
                    // remove the nh from the active list
                    active_nh_list.erase(it);
                }
                else
                {
                    it++;
                }
            }

            bank_fgnhg_map->erase(bank_member_change.nhs_to_del[del_idx]);
            syncd_fg_route_entry->active_nexthops.erase(bank_member_change.nhs_to_del[del_idx]);
            del_idx++;
        }
    }

    if (add_idx < bank_member_change.nhs_to_add.size())
    {
        uint32_t total_nhs = (uint32_t)bank_member_change.active_nhs.size() +
                         (uint32_t)bank_member_change.nhs_to_add.size() - add_idx;
        uint32_t num_buckets_in_bank = 1+ fgNhgEntry->hash_bucket_indices[syncd_bank].end_index -
            fgNhgEntry->hash_bucket_indices[syncd_bank].start_index;
        uint32_t exp_bucket_size = num_buckets_in_bank/total_nhs;
        uint32_t num_nhs_with_one_more = (num_buckets_in_bank % total_nhs);
        uint32_t num_nhs_with_eq_to_exp = total_nhs - num_nhs_with_one_more;
        uint32_t add_nh_exp_bucket_size = exp_bucket_size;


        auto active_nh_list = bank_member_change.active_nhs;
        bool move_bkt;
        bool remove_nh;

        while (add_idx < bank_member_change.nhs_to_add.size())
        {
            (*bank_fgnhg_map)[bank_member_change.nhs_to_add[add_idx]] =
                HashBuckets();
            auto it = active_nh_list.begin();
            if (num_nhs_with_eq_to_exp > 0)
            {
                num_nhs_with_eq_to_exp--;
            }
            else
            {
                add_nh_exp_bucket_size = exp_bucket_size + 1;
                num_nhs_with_one_more--;
            }
            for (const auto &nh : active_nh_list)
            {
                SWSS_LOG_INFO("Active next-hop %s has %zu buckets assigned",
                              nh.to_string().c_str(), bank_fgnhg_map->at(nh).size());
            }
            // add buckets to this nh until HashBuckets size has reached the expected size
            while (bank_fgnhg_map->at(bank_member_change.nhs_to_add[add_idx]).size() != add_nh_exp_bucket_size)
            {
                move_bkt = false;
                remove_nh = false;
                if (it == active_nh_list.end())
                {
                    it = active_nh_list.begin();
                }
                // get the active nh's hashbuckets
                HashBuckets *map_entry = &(bank_fgnhg_map->at(*it));
                /* Logic below ensure that # hash buckets assigned to a nh is equalized,
                 * we could have used simple round robin to reassign hash buckets to
                 * other available nhs, but for cases where # hash buckets is not
                 * divisible by # of nhs, simple round robin can make the hash bucket
                 * distribution non-ideal, thereby nhs can attract unequal traffic */
                if (map_entry->size() > exp_bucket_size + 1)
                {
                    SWSS_LOG_INFO("Nexthop %s has %zu, continue to remove more buckets after this.",
                                  it->to_string().c_str(), map_entry->size());
                    move_bkt=true;
                }
                else if (map_entry->size() == exp_bucket_size + 1)
                {
                    if (num_nhs_with_one_more == 0)
                    {
                        SWSS_LOG_INFO("Nexthop %s has %zu, take one bucket from it and remove it from the list.",
                            it->to_string().c_str(), map_entry->size());
                        remove_nh = true;
                        move_bkt = true;
                    }
                    else
                    {
                        SWSS_LOG_INFO("Nexthop %s with %zu buckets is one of the nexthops with one more, remove it \
                            from the list. remaining num_nhs_with_one_more = %d",
                            it->to_string().c_str(), map_entry->size(), num_nhs_with_one_more - 1);
                        remove_nh = true;
                        num_nhs_with_one_more--;
                    }
                }
                else
                {
                    SWSS_LOG_WARN("Nexthop %s already has %zu, don't remove any buckets but remove this nexthop from the list",
                        it->to_string().c_str(), map_entry->size());
                    remove_nh=true;
                }

                // Replace the active nh's buckets from the end
                if (move_bkt)
                {
                    HashBucketIdx last_elem = map_entry->at((*map_entry).size() - 1);
                    if (!writeHashBucketChange(syncd_fg_route_entry, last_elem,
                                               nhopgroup_members_set[bank_member_change.nhs_to_add[add_idx]],
                                               ipPrefix, bank_member_change.nhs_to_add[add_idx]))
                    {
                        return false;
                    }

                    (*bank_fgnhg_map)[bank_member_change.nhs_to_add[add_idx]].push_back(last_elem);
                    (*map_entry).erase((*map_entry).end() - 1);
                }

                // If done with current nexthop from the active_nh_list, then remove it. Else simply move to next nexthop
                // in the active_nh_list
                if (remove_nh)
                {
                    active_nh_list.erase(it);
                } else {
                    it++;
                }
            }
            syncd_fg_route_entry->active_nexthops.insert(bank_member_change.nhs_to_add[add_idx]);
            add_idx++;
        }
    }
    return true;
}


bool FgNhgOrch::setInactiveBankToNextAvailableActiveBank(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        uint32_t bank, std::vector<BankMemberChanges> bank_member_changes,
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    uint32_t new_bank_idx = 0;
    for (; new_bank_idx < bank_member_changes.size(); new_bank_idx++)
    {
        if (bank_member_changes[new_bank_idx].active_nhs.size() +
                bank_member_changes[new_bank_idx].nhs_to_add.size() != 0)
        {
            syncd_fg_route_entry->syncd_fgnhg_map[bank].clear();
            syncd_fg_route_entry->inactive_to_active_map[bank] = new_bank_idx;

            /* Create collated set of members which will be active in the bank */
            for (auto memb: bank_member_changes[new_bank_idx].nhs_to_add)
            {
                bank_member_changes[new_bank_idx].active_nhs.push_back(memb);
            }

            for (uint32_t i = fgNhgEntry->hash_bucket_indices[bank].start_index;
                i <= fgNhgEntry->hash_bucket_indices[bank].end_index; i++)
            {
                NextHopKey bank_nh_memb = bank_member_changes[new_bank_idx].
                         active_nhs[i % bank_member_changes[new_bank_idx].active_nhs.size()];

                if (!writeHashBucketChange(syncd_fg_route_entry, i,
                    nhopgroup_members_set[bank_nh_memb],ipPrefix, bank_nh_memb ))
                {
                    return false;
                }

                syncd_fg_route_entry->syncd_fgnhg_map[bank][bank_nh_memb].push_back(i);
            }
            break;
        }
    }

    if (new_bank_idx == bank_member_changes.size())
    {
        /* Case where there are no active banks */
        SWSS_LOG_NOTICE("All banks of FG next-hops are down for prefix %s",
                ipPrefix.to_string().c_str());

        /* This may occur when there are no neigh entries available any more
         * set route pointing to rif to allow for neigh resolution in kernel.
         * If route already points to rif then we are done.
         */
        if (!syncd_fg_route_entry->points_to_rif)
        {
            std::string interface_alias = syncd_fg_route_entry->nhg_key.getNextHops().begin()->alias;
            sai_object_id_t rif_next_hop_id = m_intfsOrch->getRouterIntfsId(interface_alias);
            if (rif_next_hop_id == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_INFO("Failed to get rif next hop for %s", interface_alias.c_str());
                return false;
            }
            if (!modifyRoutesNextHopId(gVirtualRouterId, ipPrefix, rif_next_hop_id))
            {
                SWSS_LOG_ERROR("Failed to modify route nexthopid to rif");
                return false;
            }

            if (!removeFineGrainedNextHopGroup(syncd_fg_route_entry))
            {
                SWSS_LOG_ERROR("Failed to delete Fine Grained next hop group");
                return false;
            }

            syncd_fg_route_entry->points_to_rif = true;
            syncd_fg_route_entry->next_hop_group_id = rif_next_hop_id;

            // remove state_db entry
            m_stateWarmRestartRouteTable.del(ipPrefix.to_string());
            // Clear data structures
            syncd_fg_route_entry->syncd_fgnhg_map.clear();
            syncd_fg_route_entry->active_nexthops.clear();
            syncd_fg_route_entry->inactive_to_active_map.clear();
            syncd_fg_route_entry->nhopgroup_members.clear();
        }
    }

    return true;
}


/* setInactiveBankHashBucketChanges: Sets hash buckets for inactive banks and called on a PER bank basis.
 * This function deals with scenarios where next-hop changes occurred for the route,
 * and the next-hop change causes an active bank to become inactive, or an inactive bank to become active or
 * inactive bank to remain inactive.
 * The function uses the bank member diffs provided in bank_member_changes and uses it to compute
 * the hash buckets to modify, in order to satisfy the next-hop availability for the route/neigh.
 * Eg: Lets assume prefix A had nhs 1, 2, 3, 4, 5, 6 with nhs being equally distributed over hash buckets
 * 0-59(10 per nh). Now there was a nh deletion of 1, 2, 3 which constituted bank 0(4, 5, 6 constituted bank 1)
 * This function will identify that all of bank 0's nh are down and re-assign all the hash buckets(0-29) for these nhs to
 * nhs from bank 1, along with making local struct changes to track this for future route/neigh changes.
 */
bool FgNhgOrch::setInactiveBankHashBucketChanges(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        uint32_t bank,std::vector<BankMemberChanges> &bank_member_changes,
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    if (bank_member_changes[bank].nhs_to_add.size() > 0)
    {
        /* Previously inactive bank now transitions to active */
        SWSS_LOG_INFO("Previously inactive bank now transitions to active bank %u", bank);
        syncd_fg_route_entry->syncd_fgnhg_map[bank].clear();
        for (uint32_t i = fgNhgEntry->hash_bucket_indices[bank].start_index;
                i <= fgNhgEntry->hash_bucket_indices[bank].end_index; i++)
        {
            NextHopKey bank_nh_memb = bank_member_changes[bank].
                nhs_to_add[i % bank_member_changes[bank].nhs_to_add.size()];

            if (!writeHashBucketChange(syncd_fg_route_entry, i,
                  nhopgroup_members_set[bank_nh_memb], ipPrefix, bank_nh_memb))
            {
                return false;
            }

            syncd_fg_route_entry->syncd_fgnhg_map[bank][bank_nh_memb].push_back(i);
            syncd_fg_route_entry->active_nexthops.insert(bank_nh_memb);
        }
        syncd_fg_route_entry->inactive_to_active_map[bank] = bank;

        SWSS_LOG_NOTICE("Bank# %d of FG next-hops is up for prefix %s",
                bank, ipPrefix.to_string().c_str());
    }
    else if (bank_member_changes[bank].nhs_to_del.size() > 0)
    {
        /* Previously active bank now transitions to inactive */
        SWSS_LOG_INFO("Previously active bank now transitions to inactive bank %u", bank);
        if (!setInactiveBankToNextAvailableActiveBank(syncd_fg_route_entry, fgNhgEntry,
                    bank, bank_member_changes, nhopgroup_members_set, ipPrefix))
        {
            SWSS_LOG_INFO("Failed to map to active_bank and set nh in SAI");
            return false;
        }

        for (auto memb: bank_member_changes[bank].nhs_to_del)
        {
            syncd_fg_route_entry->active_nexthops.erase(memb);
        }

        SWSS_LOG_NOTICE("Bank# %d of FG next-hops is down for prefix %s", bank,
                ipPrefix.to_string().c_str());
    }
    else
    {
        /* Previously inactive bank remains inactive */
        uint32_t active_bank = syncd_fg_route_entry->inactive_to_active_map[bank];
        SWSS_LOG_INFO("Previously inactive bank remains inactive bank %u active bank %u", bank, active_bank);
        if (bank_member_changes[active_bank].active_nhs.size() == 0)
        {
            if (!setInactiveBankToNextAvailableActiveBank(syncd_fg_route_entry, fgNhgEntry,
                        bank, bank_member_changes, nhopgroup_members_set, ipPrefix))
            {
                return false;
            }
        }
        else
        {
            if (!setActiveBankHashBucketChanges(syncd_fg_route_entry, fgNhgEntry,
                bank, bank_member_changes[active_bank], nhopgroup_members_set, ipPrefix))
            {
                return false;
            }
        }
    }
    return true;
}


bool FgNhgOrch::computeAndSetHashBucketChanges(FGNextHopGroupEntry *syncd_fg_route_entry,
        FgNhgEntry *fgNhgEntry, std::vector<BankMemberChanges> &bank_member_changes,
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set,
        const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    for (uint32_t bank_idx = 0; bank_idx < bank_member_changes.size(); bank_idx++)
    {
        if (bank_member_changes[bank_idx].active_nhs.size() != 0 ||
                (bank_member_changes[bank_idx].nhs_to_add.size() != 0 &&
                 bank_member_changes[bank_idx].nhs_to_del.size() != 0))
        {
            SWSS_LOG_INFO("active nhs in bank %u", bank_idx);
            /* Active bank is is determined by there being active nhs on the bank OR
             * an edge case where all active_nhs went down(nhs_to_del > 0) BUT
             * simultaneously, nhs were added(nhs_to_add > 0).
             * Route this to fn which deals with active banks
             */
            if (!setActiveBankHashBucketChanges(syncd_fg_route_entry, fgNhgEntry,
                        bank_idx, bank_member_changes[bank_idx], nhopgroup_members_set, ipPrefix))
            {
                return false;
            }
        }
        else
        {
            if (!setInactiveBankHashBucketChanges(syncd_fg_route_entry, fgNhgEntry,
                        bank_idx, bank_member_changes, nhopgroup_members_set, ipPrefix))
            {
                return false;
            }
        }
    }

    return true;
}


bool FgNhgOrch::setNewNhgMembers(FGNextHopGroupEntry &syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        std::vector<BankMemberChanges> &bank_member_changes, std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set,
        const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    auto num_banks = fgNhgEntry->hash_bucket_indices.size();
    std::vector<uint32_t> active_banks;
    std::vector<uint32_t> inactive_banks;

    for (uint32_t i = 0; i < num_banks; i++)
    {
        if (bank_member_changes[i].nhs_to_add.size() != 0)
        {
            active_banks.push_back(i);
            SWSS_LOG_INFO("active bank %d num nhs %zu", i, bank_member_changes[i].nhs_to_add.size());
        }
        else
        {
            inactive_banks.push_back(i);
            SWSS_LOG_INFO("inactive bank %d", i);
        }

        // add a new fgnh group map if this is a new bank
        if (i + 1 > syncd_fg_route_entry.syncd_fgnhg_map.size())
        {
            syncd_fg_route_entry.syncd_fgnhg_map.push_back(FGNextHopGroupMap());
        }
    }


    /* Case where all banks are empty, we let retry logic(upon rv false) take care of this scenario */
    if (active_banks.size() == 0)
    {
        SWSS_LOG_INFO("Found no next-hops to add, skipping");
        return false;
    }
    // spray the nhs of active banks into active bank's buckets
    for (auto active_bank : active_banks)
    {
        syncd_fg_route_entry.inactive_to_active_map[active_bank] = active_bank;
        if (!sprayBankNhgMembers(syncd_fg_route_entry, ipPrefix,
                fgNhgEntry->hash_bucket_indices[active_bank], fgNhgEntry,
                active_bank, bank_member_changes[active_bank],
                nhopgroup_members_set))
        {
            return false;
        }
    }

    // spray the nhs of active banks into inactive bank's buckets
    for (uint32_t i = 0; i < inactive_banks.size(); i++)
    {
        auto active_bank = active_banks[i % active_banks.size()];
        syncd_fg_route_entry.inactive_to_active_map[inactive_banks[i]] =
            active_bank;
        if(!sprayBankNhgMembers(syncd_fg_route_entry, ipPrefix,
                fgNhgEntry->hash_bucket_indices[inactive_banks[i]], fgNhgEntry,
                inactive_banks[i], bank_member_changes[active_bank],
                nhopgroup_members_set))
        {
            return false;
        }
        SWSS_LOG_NOTICE("Bank# %d of FG next-hops is down for prefix %s",
                inactive_banks[i], ipPrefix.to_string().c_str());
    }

    auto nexthopsMap = m_recoveryMap.find(ipPrefix.to_string());
    if (nexthopsMap != m_recoveryMap.end()) {
        m_recoveryMap.erase(nexthopsMap);
    }

    syncd_fg_route_entry.points_to_rif = false;

    return true;
}

bool FgNhgOrch::sprayBankNhgMembers(FGNextHopGroupEntry &syncd_fg_route_entry, const IpPrefix &ipPrefix,
        BankIndexRange hash_idx_range, FgNhgEntry *fgNhgEntry,
        uint32_t bank, BankMemberChanges &bank_member_change,
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set)
{
    sai_status_t status;
    bool isWarmReboot = false;
    auto nexthopsMap = m_recoveryMap.find(ipPrefix.to_string());

    SWSS_LOG_ENTER();

    // recover state before warm reboot
    if (nexthopsMap != m_recoveryMap.end())
    {
        isWarmReboot = true;
    }

    SWSS_LOG_INFO("Warm reboot is set to %d, bank %d", isWarmReboot, bank);

    // fill the hash idx range with the nhs
    for (uint32_t bucket_idx = hash_idx_range.start_index;
            bucket_idx <= hash_idx_range.end_index; bucket_idx++)
    {
        NextHopKey nh_memb_key;
        if (isWarmReboot)
        {
            nh_memb_key = nexthopsMap->second[bucket_idx];
            SWSS_LOG_INFO("Recovering nexthop %s with bucket_idx %d", nh_memb_key.ip_address.to_string().c_str(), bucket_idx);
            // case nhps in bank are all down
            if (fgNhgEntry->next_hops[nh_memb_key.ip_address].bank != bank)
            {
                syncd_fg_route_entry.inactive_to_active_map[bank] = fgNhgEntry->next_hops[nh_memb_key.ip_address].bank;
            }
        }
        else
        {
            nh_memb_key = bank_member_change.nhs_to_add[bucket_idx %
                bank_member_change.nhs_to_add.size()];
        }

        // Create a next hop group member
        sai_attribute_t nhgm_attr;
        vector<sai_attribute_t> nhgm_attrs;
        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
        nhgm_attr.value.oid = syncd_fg_route_entry.next_hop_group_id;
        nhgm_attrs.push_back(nhgm_attr);

        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
        nhgm_attr.value.oid = nhopgroup_members_set[nh_memb_key];
        nhgm_attrs.push_back(nhgm_attr);

        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_INDEX;
        nhgm_attr.value.s32 = bucket_idx;
        nhgm_attrs.push_back(nhgm_attr);

        sai_object_id_t next_hop_group_member_id;
        status = sai_next_hop_group_api->create_next_hop_group_member(
                                                          &next_hop_group_member_id,
                                                          gSwitchId,
                                                          (uint32_t)nhgm_attrs.size(),
                                                          nhgm_attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create next hop group %" PRIx64 " member %" PRIx64 ": %d",
               syncd_fg_route_entry.next_hop_group_id, next_hop_group_member_id, status);
            if (!removeFineGrainedNextHopGroup(&syncd_fg_route_entry))
            {
                SWSS_LOG_ERROR("Failed to clean-up after next-hop member creation failure");
            }

            task_process_status handle_status = handleSaiCreateStatus(SAI_API_NEXT_HOP_GROUP, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        setStateDbRouteEntry(ipPrefix, bucket_idx, nh_memb_key);
        syncd_fg_route_entry.syncd_fgnhg_map[bank][nh_memb_key].push_back(bucket_idx);
        syncd_fg_route_entry.active_nexthops.insert(nh_memb_key);
        syncd_fg_route_entry.nhopgroup_members[bucket_idx] = next_hop_group_member_id;
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    }

    return true;
}


bool FgNhgOrch::isRouteFineGrained(sai_object_id_t vrf_id, const IpPrefix &ipPrefix, const NextHopGroupKey &nextHops)
{
    SWSS_LOG_ENTER();

    if (!isFineGrainedConfigured || (vrf_id != gVirtualRouterId))
    {
        SWSS_LOG_DEBUG("Route %s:%s vrf %" PRIx64 " default_vrf %" PRIx64 " NOT fine grained ECMP",
                        ipPrefix.to_string().c_str(), nextHops.to_string().c_str(), vrf_id, gVirtualRouterId);
        return false;
    }

    FgNhgEntry *fgNhgEntry = 0;
    set<NextHopKey> next_hop_set = nextHops.getNextHops();
    auto prefix_entry = m_fgNhgPrefixes.find(ipPrefix);
    if (prefix_entry == m_fgNhgPrefixes.end())
    {
        for (NextHopKey nhk : next_hop_set)
        {
            auto member_entry = m_fgNhgNexthops.find(nhk.ip_address);
            if (member_entry == m_fgNhgNexthops.end())
            {
                if (fgNhgEntry)
                {
                    SWSS_LOG_WARN("Route %s:%s has some FG nhs, but %s is not, route is defaulted to non-fine grained ECMP",
                                ipPrefix.to_string().c_str(), nextHops.to_string().c_str(), nhk.to_string().c_str());
                }
                return false;
            }

            if (!fgNhgEntry)
            {
                fgNhgEntry = member_entry->second;
            }
            else
            {
                /* Case where fgNhgEntry is alredy found via previous nexthop
                 * We validate the it belongs to the same next-hop group set
                 */
                if (fgNhgEntry != member_entry->second)
                {
                    SWSS_LOG_INFO("FG nh found across different FG_NH groups: %s expected %s, actual %s",
                        nhk.to_string().c_str(), fgNhgEntry->fg_nhg_name.c_str(), member_entry->second->fg_nhg_name.c_str());
                    return false;
                }
            }
        }
    }
    SWSS_LOG_DEBUG("Route %s:%s vrf %" PRIx64 " default_vrf %" PRIx64 " IS fine grained ECMP",
                    ipPrefix.to_string().c_str(), nextHops.to_string().c_str(), vrf_id, gVirtualRouterId);
    return true;
}


bool FgNhgOrch::syncdContainsFgNhg(sai_object_id_t vrf_id, const IpPrefix &ipPrefix)
{
    if (!isFineGrainedConfigured || (vrf_id != gVirtualRouterId))
    {
        return false;
    }

    auto it_route_table = m_syncdFGRouteTables.find(vrf_id);
    if (it_route_table == m_syncdFGRouteTables.end())
    {
        return false;
    }

    auto it_route = it_route_table->second.find(ipPrefix);
    if (it_route == it_route_table->second.end())
    {
        return false;
    }
    return true;
}


bool FgNhgOrch::setFgNhg(sai_object_id_t vrf_id, const IpPrefix &ipPrefix, const NextHopGroupKey &nextHops,
                                    sai_object_id_t &next_hop_id, bool &isNextHopIdChanged)
{
    SWSS_LOG_ENTER();

    /* default isNextHopIdChanged to false so that sai route is unaffected
     * when we return early with success */
    isNextHopIdChanged = false;
    FgNhgEntry *fgNhgEntry = 0;
    set<NextHopKey> next_hop_set = nextHops.getNextHops();
    auto prefix_entry = m_fgNhgPrefixes.find(ipPrefix);
    if (prefix_entry != m_fgNhgPrefixes.end())
    {
        fgNhgEntry = prefix_entry->second;
    }
    else
    {
        for (NextHopKey nhk : next_hop_set)
        {
            auto member_entry = m_fgNhgNexthops.find(nhk.ip_address);
            if (member_entry == m_fgNhgNexthops.end())
            {
                SWSS_LOG_ERROR("fgNhgOrch got a route addition %s:%s for non-configured FG ECMP entry",
                                    ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
                return false;
            }
            fgNhgEntry = member_entry->second;
            break;
        }
    }

    if (m_syncdFGRouteTables.find(vrf_id) != m_syncdFGRouteTables.end() &&
        m_syncdFGRouteTables.at(vrf_id).find(ipPrefix) != m_syncdFGRouteTables.at(vrf_id).end() &&
        m_syncdFGRouteTables.at(vrf_id).at(ipPrefix).nhg_key == nextHops)
    {
        return true;
    }

    if (!fgNhgEntry)
    {
        SWSS_LOG_ERROR("fgNhgOrch got a route addition %s:%s with %s",
                ipPrefix.to_string().c_str(), nextHops.to_string().c_str(),
                (prefix_entry == m_fgNhgPrefixes.end()) ? " no prefix and FG ECMP nexthop entries" :
                "no FG ECMP entry for the prefix");
        return true;
    }

    if (m_syncdFGRouteTables.find(vrf_id) == m_syncdFGRouteTables.end())
    {
        m_syncdFGRouteTables.emplace(vrf_id, FGRouteTable());
        m_vrfOrch->increaseVrfRefCount(vrf_id);
    }

    std::map<NextHopKey,sai_object_id_t> nhopgroup_members_set;
    auto syncd_fg_route_entry_it = m_syncdFGRouteTables.at(vrf_id).find(ipPrefix);
    bool next_hop_to_add = false;

    /* Default init with # of banks */
    std::vector<BankMemberChanges> bank_member_changes(
            fgNhgEntry->hash_bucket_indices.size(), BankMemberChanges());
    if (fgNhgEntry->hash_bucket_indices.size() == 0)
    {
        /* Only happens the 1st time when hash_bucket_indices are not initialized
         */
        if (fgNhgEntry->match_mode == FGMatchMode::PREFIX_BASED)
        {
            bank_member_changes.resize(1, BankMemberChanges()); // prefix_based match_mode supports single bank
        }
        else
        {
            for (auto it : fgNhgEntry->next_hops)
            {
                while (bank_member_changes.size() <= it.second.bank)
                {
                    bank_member_changes.push_back(BankMemberChanges());
                }
            }
        }
    }

    // if match_mode==PREFIX_BASED, then initialize fgNhgEntry->next_hops with the list of IP addresses in nextHops
    if (fgNhgEntry->match_mode == FGMatchMode::PREFIX_BASED)
    {
        for (NextHopKey nhk : next_hop_set)
        {
            if (fgNhgEntry->next_hops.find(nhk.ip_address) == fgNhgEntry->next_hops.end())
            {
                if(fgNhgEntry->next_hops.size() >= fgNhgEntry->max_next_hops)
                {
                    SWSS_LOG_WARN("Next-hop %s exceeds max_next_hops %d for prefix %s, skipping",
                            nhk.to_string().c_str(), fgNhgEntry->max_next_hops, ipPrefix.to_string().c_str());
                    continue;
                }
                FGNextHopInfo fg_nh_info = {0, "", LINK_DOWN}; // prefix_based match_mode uses single bank 0
                // find link for the next hop
                if (m_neighOrch->hasNextHop(nhk))
                {
                    Port p;
                    if (gPortsOrch->getPort(nhk.alias, p))
                    {
                        // track Physical links
                        if (p.m_type == Port::PHY)
                        {
                            fg_nh_info.link = p.m_alias;
                            if (p.m_oper_status == SAI_PORT_OPER_STATUS_UP)
                            {
                                fg_nh_info.link_oper_state = LINK_UP;
                            }
                            auto link_info = fgNhgEntry->links.find(p.m_alias);
                            if (link_info != fgNhgEntry->links.end())
                            {
                                // add nhk.ip_address to the list of next-hops for the link
                                fgNhgEntry->links[p.m_alias].push_back(nhk.ip_address);
                            }
                            else {
                                // create a new list with nhk.ip_address
                                fgNhgEntry->links[p.m_alias] = {nhk.ip_address};
                            }
                        }
                    }
                    else
                    {
                        SWSS_LOG_WARN("Next-hop %s alias %s prefix %s Link not found",
                                nhk.ip_address.to_string().c_str(), nhk.alias.c_str(),
                                ipPrefix.to_string().c_str());
                    }
                }
                fgNhgEntry->next_hops[nhk.ip_address] = fg_nh_info;
                SWSS_LOG_INFO("Next-hop %s alias %s added to Fine Grained next-hop group member list for prefix %s",
                        nhk.ip_address.to_string().c_str(), nhk.alias.c_str(), ipPrefix.to_string().c_str());
            }
        }
    }

    /* Assert each IP address exists in m_syncdNextHops table,
     * and add the corresponding next_hop_id to next_hop_ids. */
    for (NextHopKey nhk : next_hop_set)
    {
        auto nexthop_entry = fgNhgEntry->next_hops.find(nhk.ip_address);
        if (!m_neighOrch->hasNextHop(nhk))
        {
            SWSS_LOG_NOTICE("Failed to get next hop %s:%s in neighorch",
                    nhk.to_string().c_str(), nextHops.to_string().c_str());
            continue;
        }
        else if (nexthop_entry == fgNhgEntry->next_hops.end())
        {
            SWSS_LOG_WARN("Could not find next-hop %s in Fine Grained next-hop group entry for prefix %s, skipping",
                    nhk.to_string().c_str(), fgNhgEntry->fg_nhg_name.c_str());
            continue;
        }
        else if (!(nexthop_entry->second.link.empty()) &&
                nexthop_entry->second.link_oper_state == LINK_DOWN)
        {
            SWSS_LOG_NOTICE("Tracked link %s associated with nh %s is down",
                    nexthop_entry->second.link.c_str(), nhk.to_string().c_str());
            continue;
        }
        else if (m_neighOrch->isNextHopFlagSet(nhk, NHFLAGS_IFDOWN))
        {
            SWSS_LOG_NOTICE("Next hop %s in %s is down, skipping",
                    nhk.to_string().c_str(), nextHops.to_string().c_str());
            continue;
        }

        if (syncd_fg_route_entry_it == m_syncdFGRouteTables.at(vrf_id).end())
        {
            bank_member_changes[fgNhgEntry->next_hops[nhk.ip_address].bank].
                nhs_to_add.push_back(nhk);
            next_hop_to_add = true;
        }
        else
        {
            FGNextHopGroupEntry *syncd_fg_route_entry = &(syncd_fg_route_entry_it->second);
            if (syncd_fg_route_entry->active_nexthops.find(nhk) ==
                syncd_fg_route_entry->active_nexthops.end())
            {
                bank_member_changes[fgNhgEntry->next_hops[nhk.ip_address].bank].
                    nhs_to_add.push_back(nhk);
                next_hop_to_add = true;
            }
        }

        sai_object_id_t nhid = m_neighOrch->getNextHopId(nhk);
        nhopgroup_members_set[nhk] = nhid;
    }

    if (syncd_fg_route_entry_it != m_syncdFGRouteTables.at(vrf_id).end())
    {
        /* Route exists and nh was associated in the past */
        FGNextHopGroupEntry *syncd_fg_route_entry = &(syncd_fg_route_entry_it->second);

        if (syncd_fg_route_entry->points_to_rif)
        {
            if (next_hop_to_add)
            {
                isNextHopIdChanged = true;
                if (!createFineGrainedNextHopGroup(*syncd_fg_route_entry, fgNhgEntry, nextHops))
                {
                    return false;
                }

                if (!setNewNhgMembers(*syncd_fg_route_entry, fgNhgEntry, bank_member_changes, nhopgroup_members_set, ipPrefix))
                {
                    return false;
                }
            }
        }
        else
        {
            /* Update FG ECMP group in SAI */
            for (auto nhk : syncd_fg_route_entry->active_nexthops)
            {
                if (nhopgroup_members_set.find(nhk) == nhopgroup_members_set.end())
                {
                    bank_member_changes[fgNhgEntry->next_hops[nhk.ip_address].bank].
                        nhs_to_del.push_back(nhk);
                }
                else
                {
                    bank_member_changes[fgNhgEntry->next_hops[nhk.ip_address].bank].
                        active_nhs.push_back(nhk);
                }
            }

            if (!computeAndSetHashBucketChanges(syncd_fg_route_entry, fgNhgEntry, bank_member_changes,
                    nhopgroup_members_set, ipPrefix))
            {
                return false;
            }
        }
    }
    else
    {
        /* New route + nhg addition */
        isNextHopIdChanged = true;
        FGNextHopGroupEntry syncd_fg_route_entry;
        if (next_hop_to_add)
        {
            if (!createFineGrainedNextHopGroup(syncd_fg_route_entry, fgNhgEntry, nextHops))
            {
                return false;
            }

            if (!setNewNhgMembers(syncd_fg_route_entry, fgNhgEntry, bank_member_changes, nhopgroup_members_set, ipPrefix))
            {
                return false;
            }
        }
        else
        {
            sai_object_id_t rif_next_hop_id = m_intfsOrch->getRouterIntfsId(next_hop_set.begin()->alias);
            if (rif_next_hop_id == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_INFO("Failed to get rif next hop %s for %s",
                        nextHops.to_string().c_str(), ipPrefix.to_string().c_str());
                return false;
            }

            syncd_fg_route_entry.next_hop_group_id = rif_next_hop_id;
            syncd_fg_route_entry.points_to_rif = true;
        }

        m_syncdFGRouteTables[vrf_id][ipPrefix] = syncd_fg_route_entry;
    }
    m_syncdFGRouteTables[vrf_id][ipPrefix].nhg_key = nextHops;

    for (uint32_t bank_idx = 0; bank_idx < bank_member_changes.size(); bank_idx++)
    {
        for (auto nh : bank_member_changes[bank_idx].nhs_to_add)
        {
            m_neighOrch->increaseNextHopRefCount(nh);
            SWSS_LOG_INFO("FG nh %s for prefix %s is up",
                    nh.to_string().c_str(), ipPrefix.to_string().c_str());
        }

        for (auto nh : bank_member_changes[bank_idx].nhs_to_del)
        {
            m_neighOrch->decreaseNextHopRefCount(nh);
            SWSS_LOG_INFO("FG nh %s for prefix %s is down",
                    nh.to_string().c_str(), ipPrefix.to_string().c_str());
        }
    }

    next_hop_id =  m_syncdFGRouteTables[vrf_id][ipPrefix].next_hop_group_id;
    return true;
}


bool FgNhgOrch::removeFgNhg(sai_object_id_t vrf_id, const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    if (!isFineGrainedConfigured)
    {
        return true;
    }

    auto it_route_table = m_syncdFGRouteTables.find(vrf_id);
    if (it_route_table == m_syncdFGRouteTables.end())
    {
        SWSS_LOG_INFO("No route table found for %s, vrf_id 0x%" PRIx64,
                ipPrefix.to_string().c_str(), vrf_id);
        return true;
    }

    auto it_route = it_route_table->second.find(ipPrefix);
    if (it_route == it_route_table->second.end())
    {
        SWSS_LOG_INFO("Failed to find route entry, vrf_id 0x%" PRIx64 ", prefix %s", vrf_id,
                ipPrefix.to_string().c_str());
        return true;
    }

    FGNextHopGroupEntry *syncd_fg_route_entry = &(it_route->second);
    if (!syncd_fg_route_entry->points_to_rif)
    {
        if (!removeFineGrainedNextHopGroup(syncd_fg_route_entry))
        {
            SWSS_LOG_ERROR("Failed to clean-up fine grained ECMP SAI group");
            return false;
        }

        for (auto nh : syncd_fg_route_entry->active_nexthops)
        {
            m_neighOrch->decreaseNextHopRefCount(nh);
        }

        // remove state_db entry
        m_stateWarmRestartRouteTable.del(ipPrefix.to_string());
    }

    it_route_table->second.erase(it_route);
    if (it_route_table->second.size() == 0)
    {
        m_syncdFGRouteTables.erase(vrf_id);
        m_vrfOrch->decreaseVrfRefCount(vrf_id);
    }
    SWSS_LOG_NOTICE("All banks of FG next-hops are down for prefix %s",
            ipPrefix.to_string().c_str());

    return true;
}


vector<FieldValueTuple> FgNhgOrch::generateRouteTableFromNhgKey(NextHopGroupKey nhg)
{
    SWSS_LOG_ENTER();
    vector<FieldValueTuple> fvVector;
    std::set<NextHopKey> nhks = nhg.getNextHops();
    string nexthops = nhks.begin()->ip_address.to_string();
    string ifnames = nhks.begin()->alias;

    for (auto nhk: nhks)
    {
        if (nhk == *(nhks.begin()))
        {
            continue;
        }
        nexthops += "," + nhk.ip_address.to_string();
        ifnames += "," + nhk.alias;
    }

    FieldValueTuple nh("nexthop", nexthops);
    FieldValueTuple idx("ifname", ifnames);
    SWSS_LOG_INFO("Generated fv nhs %s, fv ifnames %s", nexthops.c_str(), ifnames.c_str());

    fvVector.push_back(nh);
    fvVector.push_back(idx);
    return fvVector;
}


void FgNhgOrch::cleanupIpInLinkToIpMap(const string &link, const IpAddress &ip, FgNhgEntry &fgNhg_entry)
{
    SWSS_LOG_ENTER();
    if (!link.empty())
    {
        auto link_entry = fgNhg_entry.links.find(link);
        if (link_entry == fgNhg_entry.links.end())
        {
            SWSS_LOG_WARN("Unexpected case where structs are out of sync for %s",
                    link.c_str());
            return;
        }
        for (auto ip_it = begin(link_entry->second); ip_it != end(link_entry->second); ip_it++)
        {
            if (*ip_it == ip)
            {
                fgNhg_entry.links[link].erase(ip_it);
                break;
            }
        }
    }
}


bool FgNhgOrch::doTaskFgNhg(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();
    string op = kfvOp(t);
    string key = kfvKey(t);
    string fg_nhg_name = key;
    auto fgNhg_entry = m_FgNhgs.find(fg_nhg_name);
    FGMatchMode match_mode = FGMatchMode::ROUTE_BASED;
    uint32_t max_next_hops = 0;

    if (op == SET_COMMAND)
    {
        uint32_t bucket_size = 0;

        for (auto i : kfvFieldsValues(t))
        {
            if (fvField(i) == "bucket_size")
            {
                bucket_size = stoi(fvValue(i));
            }
            else if (fvField(i) == "match_mode")
            {
                if (fvValue(i) == "nexthop-based")
                {
                    match_mode = FGMatchMode::NEXTHOP_BASED;
                }
                else if (fvValue(i) == "prefix-based")
                {
                    match_mode = FGMatchMode::PREFIX_BASED;
                }
                else if (fvValue(i) != "route-based")
                {
                    SWSS_LOG_WARN("Received unsupported match_mode %s, defaulted to route-based",
                                    fvValue(i).c_str());
                }
            }
            else if (fvField(i) == "max_next_hops")
            {

                max_next_hops = stoi(fvValue(i));
            }
        }

        // Generate an error if num_next_hops is not set for prefix_based match_mode
        if (match_mode == FGMatchMode::PREFIX_BASED && max_next_hops == 0)
        {
            SWSS_LOG_ERROR("Received match_mode==prefix_based with max_next_hops 0, not a supported combination");
        }

        if (bucket_size == 0)
        {
            SWSS_LOG_ERROR("Received bucket_size which is 0 for key %s", kfvKey(t).c_str());
            return true;
        }

        if (fgNhg_entry != m_FgNhgs.end())
        {
            SWSS_LOG_WARN("FG_NHG %s already exists, ignoring", fg_nhg_name.c_str());
        }
        else
        {
            FgNhgEntry fgNhgEntry;
            fgNhgEntry.configured_bucket_size = bucket_size;
            fgNhgEntry.fg_nhg_name = fg_nhg_name;
            fgNhgEntry.match_mode = match_mode;
            fgNhgEntry.max_next_hops = max_next_hops;
            SWSS_LOG_NOTICE("Added new FG_NHG entry %s with bucket_size %d, match_mode: %'" PRIu8,
                    fg_nhg_name.c_str(), bucket_size, match_mode);
            isFineGrainedConfigured = true;
            m_FgNhgs[fg_nhg_name] = fgNhgEntry;
        }
    }
    else if (op == DEL_COMMAND)
    {
        if (fgNhg_entry == m_FgNhgs.end())
        {
            SWSS_LOG_INFO("Received delete call for non-existent entry %s",
                    fg_nhg_name.c_str());
        }
        else
        {
            /* Check if there are no child objects associated prior to deleting */
            if (fgNhg_entry->second.prefixes.size() == 0 && fgNhg_entry->second.next_hops.size() == 0)
            {
                m_FgNhgs.erase(fgNhg_entry);
                SWSS_LOG_INFO("Received delete call for valid entry with no further dependencies, deleting %s",
                        fg_nhg_name.c_str());
            }
            else
            {
                /* in case of prefix-based match mode, members are not deleted by config, so delete them here */
                if (fgNhg_entry->second.prefixes.size() == 0 &&
                        fgNhg_entry->second.match_mode == FGMatchMode::PREFIX_BASED &&
                        fgNhg_entry->second.next_hops.size() != 0)
                {
                    fgNhg_entry->second.next_hops.clear();
                    m_FgNhgs.erase(fgNhg_entry);
                    SWSS_LOG_INFO("Clearing dynamic NH Member entries associated with this FG_NHG %s",
                         fg_nhg_name.c_str());
                }
                else
                {
                    SWSS_LOG_INFO("Child Prefix/Member entries are still associated with this FG_NHG %s",
                        fg_nhg_name.c_str());
                    return false;
                }
            }
            if (m_FgNhgs.size() == 0)
            {
                isFineGrainedConfigured = false;
            }
        }
    }
    return true;
}


bool FgNhgOrch::doTaskFgNhgPrefix(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();
    string op = kfvOp(t);
    string key = kfvKey(t);
    IpPrefix ip_prefix = IpPrefix(key);
    auto prefix_entry = m_fgNhgPrefixes.find(ip_prefix);

    if (op == SET_COMMAND)
    {
        if (prefix_entry != m_fgNhgPrefixes.end())
        {
            SWSS_LOG_INFO("FG_NHG prefix already exists");
            return true;
        }

        string fg_nhg_name = "";
        for (auto i : kfvFieldsValues(t))
        {
            if (fvField(i) == "FG_NHG")
            {
                fg_nhg_name = fvValue(i);
            }
        }
        if (fg_nhg_name.empty())
        {
            SWSS_LOG_ERROR("Received FG_NHG with empty name for key %s", kfvKey(t).c_str());
            return true;
        }

        auto fgNhg_entry = m_FgNhgs.find(fg_nhg_name);
        if (fgNhg_entry == m_FgNhgs.end())
        {
            SWSS_LOG_INFO("FG_NHG entry not received yet, continue");
            return false;
        }

        // In the prefix-based match mode, only 1 FG_NHG_PREFIX is allowed for each FG_NHG since we can't
        // handle the case where multiple prefixes are attached to the same FG_NHG and we receive routes for
        // these prefixes with disjoint nexthops. If ip_prefix doesn't match with the existing prefixe in
        // the fgNhgEntry, then return false.
        if (fgNhg_entry->second.match_mode == FGMatchMode::PREFIX_BASED)
        {
            if (fgNhg_entry->second.prefixes.size() != 0)
            {
                SWSS_LOG_NOTICE("FG_NHG %s already has a prefix %s, ignoring addition of new prefix %s",
                                 fg_nhg_name.c_str(), fgNhg_entry->second.prefixes[0].to_string().c_str(),
                                 ip_prefix.to_string().c_str());
                return true;
            }
        }

        if (fgNhg_entry->second.match_mode == NEXTHOP_BASED)
        {
            SWSS_LOG_NOTICE("FG_NHG %s is configured as nexthop_based: FG_NHG_PREFIX is a no-op",
                                fg_nhg_name.c_str());
            return true;
        }

        /* delete regular ecmp handling for prefix */
        sai_object_id_t vrf_id = gVirtualRouterId;
        NextHopGroupKey nhg = gRouteOrch->getSyncdRouteNhgKey(vrf_id, ip_prefix);
        auto addCache = m_fgPrefixAddCache.find(ip_prefix);
        if (addCache == m_fgPrefixAddCache.end())
        {
            if (nhg.getSize() == 0)
            {
                SWSS_LOG_INFO("Route does not exist in routeorch, don't need to migrate route to fgnhgorch");
                fgNhg_entry->second.prefixes.push_back(ip_prefix);
                m_fgNhgPrefixes[ip_prefix] = &(fgNhg_entry->second);
            }
            else
            {
                SWSS_LOG_INFO("Route exists in routeorch, deleting from APP_DB to begin migration");
                m_fgPrefixAddCache[ip_prefix] = nhg;
                m_routeTable->del(ip_prefix.to_string());
                return false;
            }
        }
        else
        {
            if (nhg.getSize() == 0)
            {
                /* Case where APP_DB route entry was present and the route delete was completed */
                SWSS_LOG_INFO("Route removed in routeorch, now do an APP_DB addition");
                fgNhg_entry->second.prefixes.push_back(ip_prefix);
                m_fgNhgPrefixes[ip_prefix] = &(fgNhg_entry->second);
                m_routeTable->set(ip_prefix.to_string(), generateRouteTableFromNhgKey(addCache->second));
                m_fgPrefixAddCache.erase(addCache);
                SWSS_LOG_INFO("Performed APP_DB addition with prefix %s", ip_prefix.to_string().c_str());
            }
            else
            {
                SWSS_LOG_INFO("Route(%s) ADD exists in routeorch, and APP_DB route was deleted, waiting for routeorch delete to complete",
                        ip_prefix.to_string().c_str());
                return false;
            }
        }
        SWSS_LOG_INFO("FG_NHG added for group %s, prefix %s",
                m_fgNhgPrefixes[ip_prefix]->fg_nhg_name.c_str(), ip_prefix.to_string().c_str());
    }
    else if (op == DEL_COMMAND)
    {
        if (prefix_entry == m_fgNhgPrefixes.end())
        {
            SWSS_LOG_INFO("FG_NHG prefix doesn't exists, ignore");
            return true;
        }
        /* delete fine grained ecmp handling for prefix */
        auto fgNhg_entry = prefix_entry->second;
        sai_object_id_t vrf_id = gVirtualRouterId;
        NextHopGroupKey nhg;
        if (m_syncdFGRouteTables.find(vrf_id) != m_syncdFGRouteTables.end() &&
                    m_syncdFGRouteTables.at(vrf_id).find(ip_prefix) != m_syncdFGRouteTables.at(vrf_id).end())
        {
            nhg = m_syncdFGRouteTables.at(vrf_id).at(ip_prefix).nhg_key;
        }

        auto delCache = m_fgPrefixDelCache.find(ip_prefix);
        if (delCache == m_fgPrefixDelCache.end())
        {
            if (nhg.getSize() == 0)
            {
                SWSS_LOG_INFO("Route(%s) does not exist in fgnhgorch, proceed with deletion of local structures",
                        ip_prefix.to_string().c_str());

                for (uint32_t i = 0; i < fgNhg_entry->prefixes.size(); i++)
                {
                    if(fgNhg_entry->prefixes[i] == ip_prefix)
                    {
                        fgNhg_entry->prefixes.erase(fgNhg_entry->prefixes.begin() + i);
                        break;
                    }
                }

                m_fgNhgPrefixes.erase(ip_prefix);
            }
            else
            {
                SWSS_LOG_INFO("Route(%s) exists in fgNhgOrch, deleting from APP_DB", ip_prefix.to_string().c_str());
                m_fgPrefixDelCache[ip_prefix] = nhg;
                m_routeTable->del(ip_prefix.to_string());
                return false;
            }
        }
        else
        {
            if (nhg.getSize() == 0)
            {
                /* Case where fgnhgorch route entry was present and the route delete was completed */
                SWSS_LOG_INFO("Route(%s) removed in fgNhgOrch, now do an APP_DB addition", ip_prefix.to_string().c_str());
                for (uint32_t i = 0; i < fgNhg_entry->prefixes.size(); i++)
                {
                    if (fgNhg_entry->prefixes[i] == ip_prefix)
                    {
                        fgNhg_entry->prefixes.erase(fgNhg_entry->prefixes.begin() + i);
                        break;
                    }
                }
                m_fgNhgPrefixes.erase(ip_prefix);

                m_routeTable->set(ip_prefix.to_string(), generateRouteTableFromNhgKey(delCache->second));
                m_fgPrefixDelCache.erase(delCache);
                SWSS_LOG_INFO("Perform APP_DB addition with prefix %s", ip_prefix.to_string().c_str());
            }
            else
            {
                SWSS_LOG_INFO("Route(%s) DEL exists in fgNhgOrch size(%zu), and APP_DB route was deleted, waiting for fgNhgOrch delete to complete!!",
                        ip_prefix.to_string().c_str(), nhg.getSize());
                return false;
            }
        }
        SWSS_LOG_INFO("FG_NHG removed for group prefix %s", ip_prefix.to_string().c_str());

    }
    return true;
}


bool FgNhgOrch::doTaskFgNhgMember(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();
    string op = kfvOp(t);
    string key = kfvKey(t);
    IpAddress next_hop = IpAddress(key);
    NextHopKey nhk(next_hop.to_string());
    bool link_oper = LINK_UP;

    if (op == SET_COMMAND)
    {
        string fg_nhg_name = "";
        uint32_t bank = 0;
        string link = "";
        for (auto i : kfvFieldsValues(t))
        {
            if (fvField(i) == "FG_NHG")
            {
                fg_nhg_name = fvValue(i);
            }
            else if (fvField(i) == "bank")
            {
                bank = stoi(fvValue(i));
            }
            else if (fvField(i) == "link")
            {
                link = fvValue(i);
            }
        }
        if (fg_nhg_name.empty())
        {
            SWSS_LOG_ERROR("Received FG_NHG with empty name for key %s", kfvKey(t).c_str());
            return true;
        }

        auto fgNhg_entry = m_FgNhgs.find(fg_nhg_name);
        if (fgNhg_entry == m_FgNhgs.end())
        {
            SWSS_LOG_INFO("FG_NHG entry not received yet, continue");
            return false;
        }
        // Flag error if FGNHG was created with match_mode as prefix_based and member is added
        else if (fgNhg_entry->second.match_mode == FGMatchMode::PREFIX_BASED)
        {
            SWSS_LOG_ERROR("Received FG_NHG member for prefix-based match_mode, not a supported operation");
            return true;
        }
        else
        {
            /* skip addition if next-hop already exists */
            if (fgNhg_entry->second.next_hops.find(next_hop) != fgNhg_entry->second.next_hops.end())
            {
                SWSS_LOG_INFO("FG_NHG member %s already exists for %s, skip",
                        next_hop.to_string().c_str(), fg_nhg_name.c_str());
                return true;
            }
            FGNextHopInfo fg_nh_info = {};
            fg_nh_info.bank = bank;

            if (!link.empty())
            {
                /* Identify link oper state for initialization */
                Port p;
                if (!gPortsOrch->getPort(link, p))
                {
                    SWSS_LOG_WARN("FG_NHG member %s added to %s with non-existent link %s, link mapping skipped",
                            next_hop.to_string().c_str(), fg_nhg_name.c_str(), link.c_str());
                }
                else
                {
                    link_oper = LINK_DOWN; /* Default operational state is down */
                    fg_nh_info.link = link;
                    if (p.m_oper_status == SAI_PORT_OPER_STATUS_UP)
                    {
                        link_oper = LINK_UP;
                    }
                    auto link_info = fgNhg_entry->second.links.find(link);
                    fg_nh_info.link_oper_state = link_oper;

                    if (link_info != fgNhg_entry->second.links.end())
                    {
                        link_info->second.push_back(next_hop);
                    }
                    else
                    {
                        std::vector<IpAddress> ips;
                        ips.push_back(next_hop);
                        fgNhg_entry->second.links[link] = ips;
                    }
                    SWSS_LOG_INFO("Added link %s to ip %s map", link.c_str(), key.c_str());
                }
            }

            fgNhg_entry->second.next_hops[next_hop] = fg_nh_info;

            if (fgNhg_entry->second.match_mode == NEXTHOP_BASED)
            {
                SWSS_LOG_NOTICE("Add member %s as NEXTHOP_BASED", next_hop.to_string().c_str());
                m_fgNhgNexthops[next_hop] = &(fgNhg_entry->second);
            }

            /* query and check the next hop is valid in neighOrcch */
            if (!m_neighOrch->hasNextHop(nhk))
            {
                SWSS_LOG_INFO("Nexthop %s is not resolved yet", nhk.to_string().c_str());
            }
            else if (link_oper)
            {
                /* add next-hop into SAI group if associated link is up/no link associated with this nh */
                if (!validNextHopInNextHopGroup(nhk))
                {
                    cleanupIpInLinkToIpMap(link, next_hop, fgNhg_entry->second);
                    fgNhg_entry->second.next_hops.erase(next_hop);
                    m_fgNhgNexthops.erase(next_hop);
                    SWSS_LOG_INFO("Failing validNextHopInNextHopGroup for %s", nhk.to_string().c_str());
                    return false;
                }
            }

            SWSS_LOG_INFO("FG_NHG member added for group %s, next-hop %s",
                    fgNhg_entry->second.fg_nhg_name.c_str(), nhk.to_string().c_str());
        }
    }
    else if (op == DEL_COMMAND)
    {
        /* remove next hop from SAI group if its a resolved nh which is programmed to SAI*/
        if (m_neighOrch->hasNextHop(nhk))
        {
            if (!invalidNextHopInNextHopGroup(nhk))
            {
                return false;
            }
        }

        SWSS_LOG_INFO("FG_NHG member removed for SAI group, next-hop %s",
                next_hop.to_string().c_str());

        /* remove next-hop in fgnhg entry*/
        for (auto fgnhg_it = m_FgNhgs.begin(); fgnhg_it != m_FgNhgs.end(); ++fgnhg_it)
        {
            auto it = fgnhg_it->second.next_hops.find(next_hop);
            if (it != fgnhg_it->second.next_hops.end())
            {
                string link = it->second.link;
                cleanupIpInLinkToIpMap(link, next_hop, fgnhg_it->second);
                fgnhg_it->second.next_hops.erase(it);
                SWSS_LOG_INFO("FG_NHG member removed for group %s, next-hop %s",
                    fgnhg_it->second.fg_nhg_name.c_str(), next_hop.to_string().c_str());
                break;
            }
        }
        m_fgNhgNexthops.erase(next_hop);
    }
    return true;
}


void FgNhgOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    const string & table_name = consumer.getTableName();
    auto it = consumer.m_toSync.begin();
    bool entry_handled = true;

    while (it != consumer.m_toSync.end())
    {
        auto t = it->second;
        if (table_name == CFG_FG_NHG)
        {
            entry_handled = doTaskFgNhg(t);
        }
        else if (table_name == CFG_FG_NHG_PREFIX)
        {
            entry_handled = doTaskFgNhgPrefix(t);
        }
        else if (table_name == CFG_FG_NHG_MEMBER)
        {
            entry_handled = doTaskFgNhgMember(t);
        }
        else
        {
            entry_handled = true;
            SWSS_LOG_ERROR("Unknown table : %s", table_name.c_str());
        }

        if (entry_handled)
        {
            consumer.m_toSync.erase(it++);
        }
        else
        {
            it++;
        }
    }
    return;
}
