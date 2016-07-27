/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/bgp_path.h"

#include <boost/foreach.hpp>

#include "bgp/bgp_route.h"
#include "bgp/bgp_peer.h"
#include "net/community_type.h"

using std::string;
using std::vector;

string BgpPath::PathIdString(uint32_t path_id) {
    Ip4Address addr(path_id);
    return addr.to_string();
}

BgpPath::BgpPath(const IPeer *peer, uint32_t path_id, PathSource src,
                 const BgpAttrPtr ptr, uint32_t flags, uint32_t label)
    : peer_(peer), path_id_(path_id), source_(src), attr_(ptr),
      original_attr_(ptr), flags_(flags), label_(label) {
}

BgpPath::BgpPath(const IPeer *peer, PathSource src, const BgpAttrPtr ptr,
        uint32_t flags, uint32_t label)
    : peer_(peer), path_id_(0), source_(src), attr_(ptr), original_attr_(ptr),
      flags_(flags), label_(label) {
}

BgpPath::BgpPath(uint32_t path_id, PathSource src, const BgpAttrPtr ptr,
        uint32_t flags, uint32_t label)
    : peer_(NULL), path_id_(path_id), source_(src), attr_(ptr),
      original_attr_(ptr), flags_(flags), label_(label) {
}

BgpPath::BgpPath(PathSource src, const BgpAttrPtr ptr,
        uint32_t flags, uint32_t label)
    : peer_(NULL), path_id_(0), source_(src), attr_(ptr), original_attr_(ptr),
      flags_(flags), label_(label) {
}

// True is better
#define BOOL_COMPARE(CondA, CondB)   \
    do {                                \
        if (CondA) {                    \
            if (!(CondB)) return -1;    \
        } else {                        \
            if (CondB) return 1;        \
        }                               \
    } while (0)

int BgpPath::PathCompare(const BgpPath &rhs, bool allow_ecmp) const {
    const BgpAttr *rattr = rhs.GetAttr();

    // Feasible Path first
    KEY_COMPARE(rhs.IsFeasible(), IsFeasible());

    // Compare local_pref in reverse order as larger is better.
    KEY_COMPARE(rattr->local_pref(), attr_->local_pref());

    // Compare sequence_number in reverse order as larger is better.
    KEY_COMPARE(rattr->sequence_number(), attr_->sequence_number());

    // Route without LLGR_STALE community is always preferred over one with.
    bool llgr_stale = attr_->community() && attr_->community()->ContainsValue(
                                                CommunityType::LlgrStale);
    llgr_stale |= IsLlgrStale();

    bool rllgr_stale = rattr->community() && rattr->community()->ContainsValue(
                                                 CommunityType::LlgrStale);
    rllgr_stale |= rhs.IsLlgrStale();

    KEY_COMPARE(llgr_stale, rllgr_stale);

    // Do not compare as path length for service chain paths at this point.
    // We want to treat service chain paths as ECMP irrespective of as path
    // length.
    if (!attr_->origin_vn_path() || !rattr->origin_vn_path())
        KEY_COMPARE(attr_->as_path_count(), rattr->as_path_count());

    KEY_COMPARE(attr_->origin(), rattr->origin());

    // Compare med if both paths are learnt from the same neighbor as.
    if (attr_->neighbor_as() && attr_->neighbor_as() == rattr->neighbor_as())
        KEY_COMPARE(attr_->med(), rattr->med());

    // For ECMP paths, above checks should suffice.
    if (allow_ecmp)
        return 0;

    // Compare as path length for service chain paths since we bypassed the
    // check previously.
    if (attr_->origin_vn_path() && rattr->origin_vn_path())
        KEY_COMPARE(attr_->as_path_count(), rattr->as_path_count());

    // Prefer locally generated routes over bgp and xmpp routes.
    BOOL_COMPARE(peer_ == NULL, rhs.peer_ == NULL);

    // Compare the source and the path id.
    KEY_COMPARE(rhs.GetSource(), GetSource());

    // Bail if both paths are local since all subsequent checks are
    // based on IPeer properties.
    if (peer_ == NULL && rhs.peer_ == NULL) {
        KEY_COMPARE(path_id_, rhs.path_id_);
        return 0;
    }

    // Prefer xmpp routes over bgp routes.
    BOOL_COMPARE(peer_->IsXmppPeer(), rhs.peer_->IsXmppPeer());

    KEY_COMPARE(path_id_, rhs.path_id_);

    // Path received from EBGP is better than the one received from IBGP
    KEY_COMPARE(peer_->PeerType() == BgpProto::IBGP,
                rhs.peer_->PeerType() == BgpProto::IBGP);

    // Lower router id is better. Substitute originator id for router id
    // if the path has an originator id.
    uint32_t orig_id = attr_->originator_id().to_ulong();
    uint32_t rorig_id = rattr->originator_id().to_ulong();
    uint32_t id = orig_id ? orig_id : peer_->bgp_identifier();
    uint32_t rid = rorig_id ? rorig_id : rhs.peer_->bgp_identifier();
    KEY_COMPARE(id, rid);

    KEY_COMPARE(attr_->cluster_list_length(), rattr->cluster_list_length());

    const BgpPeer *lpeer = dynamic_cast<const BgpPeer *>(peer_);
    const BgpPeer *rpeer = dynamic_cast<const BgpPeer *>(rhs.peer_);
    if (lpeer != NULL && rpeer != NULL) {
        KEY_COMPARE(lpeer->peer_key(), rpeer->peer_key());
    }

    return 0;
}

bool BgpPath::PathSameNeighborAs(const BgpPath &rhs) const {
    const BgpAttr *rattr = rhs.GetAttr();
    if (!peer_ || peer_->PeerType() != BgpProto::EBGP)
        return false;
    if (!rhs.peer_ || rhs.peer_->PeerType() != BgpProto::EBGP)
        return false;
    return (attr_->neighbor_as() == rattr->neighbor_as());
}

void BgpPath::UpdatePeerRefCount(int count) const {
    if (!peer_)
        return;
    peer_->UpdateTotalPathCount(count);
    if (source_ != BGP_XMPP || IsReplicated())
        return;
    peer_->UpdatePrimaryPathCount(count);
}

string BgpPath::ToString() const {
    return peer_ ? peer_->ToString() : "Nil";
}

RouteDistinguisher BgpPath::GetSourceRouteDistinguisher() const {
    if (!attr_->source_rd().IsZero())
        return attr_->source_rd();
    if (!IsReplicated())
        return RouteDistinguisher::kZeroRd;

    const BgpSecondaryPath *path = static_cast<const BgpSecondaryPath *>(this);
    return path->GetPrimaryRouteDistinguisher();
}

vector<string> BgpPath::GetFlagsStringList() const {
    vector<string> flag_names;
    if (flags_ == 0) {
        flag_names.push_back("None");
        return flag_names;
    }

    // First we form a list of enums and then iterate over it to get their
    // string forms using switch. This lets compiler tell us when ever we add a
    // new enumeration to PathFlag.
    vector<PathFlag> flags;
    if (flags_ & AsPathLooped)
        flags.push_back(AsPathLooped);
    if (flags_ & NoNeighborAs)
        flags.push_back(NoNeighborAs);
    if (flags_ & Stale)
        flags.push_back(Stale);
    if (flags_ & NoTunnelEncap)
        flags.push_back(NoTunnelEncap);
    if (flags_ & OriginatorIdLooped)
        flags.push_back(OriginatorIdLooped);
    if (flags_ & ResolveNexthop)
        flags.push_back(ResolveNexthop);
    if (flags_ & ResolvedPath)
        flags.push_back(ResolvedPath);
    if (flags_ & RoutingPolicyReject)
        flags.push_back(RoutingPolicyReject);
    if (flags_ & LlgrStale)
        flags.push_back(LlgrStale);

    BOOST_FOREACH(PathFlag flag, flags) {
        switch (flag) {
        case AsPathLooped:
            flag_names.push_back("AsPathLooped");
            break;
        case NoNeighborAs:
            flag_names.push_back("NoNeighborAs");
            break;
        case Stale:
            flag_names.push_back("Stale");
            break;
        case NoTunnelEncap:
            flag_names.push_back("NoTunnelEncap");
            break;
        case OriginatorIdLooped:
            flag_names.push_back("OriginatorIdLooped");
            break;
        case ResolveNexthop:
            flag_names.push_back("ResolveNexthop");
            break;
        case ResolvedPath:
            flag_names.push_back("ResolvedPath");
            break;
        case RoutingPolicyReject:
            flag_names.push_back("RoutingPolicyReject");
            break;
        case LlgrStale:
            flag_names.push_back("LlgrStale");
            break;
        }
    }
    return flag_names;
}

string BgpPath::GetSourceString(bool combine_bgp_and_xmpp) const {
    switch (source_) {
    case None:
        return "None";
    case BGP_XMPP:
        if (combine_bgp_and_xmpp) {
            return "BGP_XMPP";
        } else if (peer_) {
            return(peer_->IsXmppPeer() ? "XMPP" : "BGP");
        } else {
            return "None";
        }
        break;
    case ServiceChain:
        return "ServiceChain";
    case StaticRoute:
        return "StaticRoute";
    case Aggregate:
        return "Aggregate";
    case Local:
        return "Local";
    default:
        break;
    }
    return "None";
}

BgpSecondaryPath::BgpSecondaryPath(const IPeer *peer, uint32_t path_id,
        PathSource src, const BgpAttrPtr ptr, uint32_t flags, uint32_t label)
    : BgpPath(peer, path_id, src, ptr, flags, label) {
}

RouteDistinguisher BgpSecondaryPath::GetPrimaryRouteDistinguisher() const {
    return src_entry_->GetRouteDistinguisher();
}
