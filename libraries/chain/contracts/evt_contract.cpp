/**
 *  @file
 *  @copyright defined in evt/LICENSE.txt
 */
#include <evt/chain/contracts/evt_contract.hpp>

#include <algorithm>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/ripemd160.hpp>
#include <evt/chain/apply_context.hpp>
#include <evt/chain/token_database.hpp>
#include <evt/chain/transaction_context.hpp>
#include <evt/chain/contracts/types.hpp>
#include <evt/utilities/safemath.hpp>

namespace evt { namespace chain { namespace contracts {

#define EVT_ACTION_IMPL(name)                         \
    template<>                                        \
    struct apply_action<name> {                       \
        static void invoke(apply_context&);           \
    };                                                \
    template struct apply_action<name>;               \
                                                      \
    void                                              \
    apply_action<name>::invoke(apply_context& context)

namespace __internal {

inline bool 
validate(const permission_def &permission) {
    uint32_t total_weight = 0;
    for(const auto& aw : permission.authorizers) {
        if(aw.weight == 0) {
            return false;
        }
        total_weight += aw.weight;
    }
    return total_weight >= permission.threshold;
}

inline bool
validate(const group& group, const group::node& node) {
    EVT_ASSERT(node.validate(), group_type_exception, "Node is invalid: ${node}", ("node",node));
    if(!node.is_leaf()) {
        auto total_weight = 0u;
        auto result = true;
        group.visit_node(node, [&](auto& n) {
            if(!validate(group, n)) {
                result = false;
                return false;
            } 
            total_weight += n.weight;
            return true;
        });
        if(!result) {
            return false;
        }
        return total_weight >= node.threshold;
    }
    return true;
}

inline bool
validate(const group& group) {
    EVT_ASSERT(!group.name().empty(), group_type_exception, "Group name cannot be empty.");
    EVT_ASSERT(!group.empty(), group_type_exception, "Root node does not exist.");
    auto& root = group.root();
    return validate(group, root);
}

auto make_permission_checker = [](const auto& tokendb) {
    auto checker = [&](const auto& p, auto allowed_owner) {
        for(const auto& a : p.authorizers) {
            auto& ref = a.ref;

            switch(ref.type()) {
            case authorizer_ref::account_t: {
                continue;
            }
            case authorizer_ref::owner_t: {
                EVT_ASSERT(allowed_owner, permission_type_exception, "Owner group does not show up in ${name} permission, and it only appears in Transfer.", ("name", p.name));
                continue;  
            }
            case authorizer_ref::group_t: {
                auto& name = ref.get_group();
                auto dbexisted = tokendb.exists_group(name);
                EVT_ASSERT(dbexisted, group_not_existed_exception, "Group ${name} does not exist.", ("name", name));
                break;
            }
            default: {
                EVT_ASSERT(false, authorizer_ref_type_exception, "Authorizer ref is not valid.");
            }
            }  // switch
        }
    };
    return checker;
};

inline void
check_name_reserved(const name128& name) {
    const uint128_t reserved_flag = ((uint128_t)0x3f << (128-6));
    EVT_ASSERT(!name.empty() && (name.value & reserved_flag), name_reserved_exception, "Name starting with '.' is reserved for system usages.");
}

} // namespace __internal

EVT_ACTION_IMPL(newdomain) {
    using namespace __internal;

    auto ndact = context.act.data_as<newdomain>();
    try {
        EVT_ASSERT(context.has_authorized(ndact.name, N128(.create)), action_authorize_exception, "Authorized information does not match.");

        check_name_reserved(ndact.name);

        auto& tokendb = context.token_db;
        EVT_ASSERT(!tokendb.exists_domain(ndact.name), domain_exists_exception, "Domain ${name} already exists.", ("name",ndact.name));

        EVT_ASSERT(ndact.issue.name == "issue", permission_type_exception, "Name ${name} does not match with the name of issue permission.", ("name",ndact.issue.name));
        EVT_ASSERT(ndact.issue.threshold > 0 && validate(ndact.issue), permission_type_exception, "Issue permission is not valid, which may be caused by invalid threshold, duplicated keys or unordered keys.");
        EVT_ASSERT(ndact.transfer.name == "transfer", permission_type_exception, "Name ${name} does not match with the name of transfer permission.", ("name",ndact.transfer.name));
        EVT_ASSERT(ndact.transfer.threshold > 0 && validate(ndact.transfer), permission_type_exception, "Transfer permission is not valid, which may be caused by invalid threshold, duplicated keys or unordered keys.");
        // manage permission's threshold can be 0 which means no one can update permission later.
        EVT_ASSERT(ndact.manage.name == "manage", permission_type_exception, "Name ${name} does not match with the name of manage permission.", ("name",ndact.manage.name));
        EVT_ASSERT(validate(ndact.manage), permission_type_exception, "Manage permission is not valid, which may be caused by duplicated keys.");

        auto pchecker = make_permission_checker(tokendb);
        pchecker(ndact.issue, false);
        pchecker(ndact.transfer, true);
        pchecker(ndact.manage, false);

        domain_def domain;
        domain.name        = ndact.name;
        domain.creator     = ndact.creator;
        domain.create_time = context.control.head_block_time();
        domain.issue       = std::move(ndact.issue);
        domain.transfer    = std::move(ndact.transfer);
        domain.manage      = std::move(ndact.manage);
        
        tokendb.add_domain(domain);       
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

EVT_ACTION_IMPL(issuetoken) {
    using namespace __internal;

    auto itact = context.act.data_as<issuetoken>();
    try {
        EVT_ASSERT(context.has_authorized(itact.domain, N128(.issue)), action_authorize_exception, "Authorized information does not match.");
        EVT_ASSERT(!itact.owner.empty(), token_owner_exception, "Owner cannot be empty.");

        auto check_owner = [](const auto& addr) {
            EVT_ASSERT(addr.is_public_key(), token_owner_exception, "Owner should be public key address");
        };
        for(auto& addr : itact.owner) {
            check_owner(addr);
        }

        auto& tokendb = context.token_db;
        EVT_ASSERT(tokendb.exists_domain(itact.domain), domain_not_existed_exception, "Domain ${name} does not exist.", ("name", itact.domain));

        auto check_name = [&](const auto& name) {
            check_name_reserved(name);
            EVT_ASSERT(!tokendb.exists_token(itact.domain, name), token_exists_exception, "Token ${domain}-${name} already exists.", ("domain",itact.domain)("name",name));
        };
        for(auto& n : itact.names) {
            check_name(n);
        }

        tokendb.issue_tokens(itact);
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

namespace __internal {

bool
check_token_destroy(const token_def& token) {
    if(token.owner.size() != 1) {
        return false;
    }
    return token.owner[0].is_reserved();
}

}  // namespace __internal

EVT_ACTION_IMPL(transfer) {
    using namespace __internal;

    auto ttact = context.act.data_as<transfer>();
    try {
        EVT_ASSERT(context.has_authorized(ttact.domain, ttact.name), action_authorize_exception, "Authorized information does not match.");
        EVT_ASSERT(!ttact.to.empty(), token_owner_exception, "New owner cannot be empty.");

        auto check_owner = [](const auto& addr) {
            EVT_ASSERT(addr.is_public_key(), token_owner_exception, "Owner should be public key address");
        };
        for(auto& addr : ttact.to) {
            check_owner(addr);
        }

        auto& tokendb = context.token_db;

        token_def token;
        tokendb.read_token(ttact.domain, ttact.name, token);

        EVT_ASSERT(!check_token_destroy(token), token_destoryed_exception, "Token is already destroyed.");

        token.owner = std::move(ttact.to);
        tokendb.update_token(token);
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

EVT_ACTION_IMPL(destroytoken) {
    using namespace __internal;

    auto dtact = context.act.data_as<destroytoken>();
    try {
        EVT_ASSERT(context.has_authorized(dtact.domain, dtact.name), action_authorize_exception, "Authorized information does not match.");

        auto& tokendb = context.token_db;

        token_def token;
        tokendb.read_token(dtact.domain, dtact.name, token);

        EVT_ASSERT(!check_token_destroy(token), token_destoryed_exception, "Token is already destroyed.");

        token.owner = address_list{ address() };
        tokendb.update_token(token);
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

EVT_ACTION_IMPL(newgroup) {
    using namespace __internal;

    auto ngact = context.act.data_as<newgroup>();
    try {
        EVT_ASSERT(context.has_authorized(N128(.group), ngact.name), action_authorize_exception, "Authorized information does not match.");
        EVT_ASSERT(!ngact.group.key().is_generated(), group_key_exception, "Group key cannot be generated key");
        EVT_ASSERT(ngact.name == ngact.group.name(), group_name_exception, "Group name not match, act: ${n1}, group: ${n2}", ("n1",ngact.name)("n2",ngact.group.name()));
        
        check_name_reserved(ngact.name);
        
        auto& tokendb = context.token_db;
        EVT_ASSERT(!tokendb.exists_group(ngact.name), group_exists_exception, "Group ${name} already exists.", ("name",ngact.name));
        EVT_ASSERT(validate(ngact.group), group_type_exception, "Input group is not valid.");

        tokendb.add_group(std::move(ngact.group));
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

EVT_ACTION_IMPL(updategroup) {
    using namespace __internal;

    auto ugact = context.act.data_as<updategroup>();
    try {
        EVT_ASSERT(context.has_authorized(N128(.group), ugact.name), action_authorize_exception, "Authorized information does not match.");
        EVT_ASSERT(ugact.name == ugact.group.name(), group_name_exception, "Names in action are not the same.");

        auto& tokendb = context.token_db;
        
        group_def group;
        tokendb.read_group(ugact.name, group);
        
        EVT_ASSERT(!group.key().is_reserved(), group_key_exception, "Reserved group key cannot be used to udpate group");
        EVT_ASSERT(validate(ugact.group), group_type_exception, "Updated group is not valid.");

        tokendb.update_group(std::move(ugact.group));
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

EVT_ACTION_IMPL(updatedomain) {
    using namespace __internal;

    auto udact = context.act.data_as<updatedomain>();
    try {
        EVT_ASSERT(context.has_authorized(udact.name, N128(.update)), action_authorize_exception, "Authorized information does not match");

        auto& tokendb = context.token_db;

        domain_def domain;
        tokendb.read_domain(udact.name, domain);

        auto pchecker = make_permission_checker(tokendb);
        if(udact.issue.valid()) {
            EVT_ASSERT(udact.issue->name == "issue", permission_type_exception, "Name ${name} does not match with the name of issue permission.", ("name",udact.issue->name));
            EVT_ASSERT(udact.issue->threshold > 0 && validate(*udact.issue), permission_type_exception, "Issue permission is not valid, which may be caused by invalid threshold, duplicated keys or unordered keys.");
            pchecker(*udact.issue, false);

            domain.issue = std::move(*udact.issue);
        }
        if(udact.transfer.valid()) {
            EVT_ASSERT(udact.transfer->name == "transfer", permission_type_exception, "Name ${name} does not match with the name of transfer permission.", ("name",udact.transfer->name));
            EVT_ASSERT(udact.transfer->threshold > 0 && validate(*udact.transfer), permission_type_exception, "Transfer permission is not valid, which may be caused by invalid threshold, duplicated keys or unordered keys.");
            pchecker(*udact.transfer, true);

            domain.transfer = std::move(*udact.transfer);
        }
        if(udact.manage.valid()) {
            // manage permission's threshold can be 0 which means no one can update permission later.
            EVT_ASSERT(udact.manage->name == "manage", permission_type_exception, "Name ${name} does not match with the name of manage permission.", ("name",udact.manage->name));
            EVT_ASSERT(validate(*udact.manage), permission_type_exception, "Manage permission is not valid, which may be caused by duplicated keys.");
            pchecker(*udact.manage, false);

            domain.manage = std::move(*udact.manage);
        }

        tokendb.update_domain(domain);
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

namespace __internal {

address
get_fungible_address(symbol sym) {
    return address(N(fungible), (fungible_name)sym.name(), 0);
}

void
transfer_fungible(asset& from, asset& to, uint64_t total) {
    bool r1, r2;
    decltype(from.get_amount()) r;

    r1 = safemath::test_sub(from.get_amount(), total, r);
    r2 = safemath::test_add(to.get_amount(), total, r);
    EVT_ASSERT(r1 && r2, math_overflow_exception, "Opeartions resulted in overflows.");
    
    from -= asset(total, from.get_symbol());
    to += asset(total, to.get_symbol());
}

}  // namespace __internal

EVT_ACTION_IMPL(newfungible) {
    using namespace __internal;

    auto nfact = context.act.data_as<newfungible>();
    try {
        EVT_ASSERT(context.has_authorized(N128(.fungible), (fungible_name)nfact.sym.name()), action_authorize_exception, "Authorized information does not match.");
        EVT_ASSERT(nfact.total_supply.get_amount() > 0, fungible_supply_exception, "Total supply cannot be zero");

        auto& tokendb = context.token_db;
        EVT_ASSERT(!tokendb.exists_fungible(nfact.sym), fungible_exists_exception, "Fungible with symbol: ${sym} already exists.", ("sym",nfact.sym.name()));
        EVT_ASSERT(nfact.sym == nfact.total_supply.get_symbol(), fungible_symbol_exception, "Symbols are not the same.");
        EVT_ASSERT(nfact.total_supply.get_amount() <= ASSET_MAX_SHARE_SUPPLY, fungible_supply_exception, "Supply exceeds the maximum allowed.");

        EVT_ASSERT(nfact.issue.name == "issue", permission_type_exception, "Name ${name} does not match with the name of issue permission.", ("name",nfact.issue.name));
        EVT_ASSERT(nfact.issue.threshold > 0 && validate(nfact.issue), permission_type_exception, "Issue permission is not valid, which may be caused by invalid threshold, duplicated keys or unordered keys.");
        // manage permission's threshold can be 0 which means no one can update permission later.
        EVT_ASSERT(nfact.manage.name == "manage", permission_type_exception, "Name ${name} does not match with the name of manage permission.", ("name",nfact.manage.name));
        EVT_ASSERT(validate(nfact.manage), permission_type_exception, "Manage permission is not valid, which may be caused by duplicated keys.");

        auto pchecker = make_permission_checker(tokendb);
        pchecker(nfact.issue, false);
        pchecker(nfact.manage, false);

        fungible_def fungible;
        fungible.sym            = nfact.sym;
        fungible.creator        = nfact.creator;
        fungible.create_time    = context.control.head_block_time();
        fungible.issue          = std::move(nfact.issue);
        fungible.manage         = std::move(nfact.manage);
        fungible.total_supply   = nfact.total_supply;

        tokendb.add_fungible(fungible);

        auto addr = get_fungible_address(nfact.sym);
        tokendb.update_asset(addr, nfact.total_supply);
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

EVT_ACTION_IMPL(updfungible) {
    using namespace __internal;

    auto ufact = context.act.data_as<updfungible>();
    try {
        EVT_ASSERT(context.has_authorized(N128(.fungible), (fungible_name)ufact.sym.name()), action_authorize_exception, "Authorized information does not match.");

        auto& tokendb = context.token_db;

        fungible_def fungible;
        tokendb.read_fungible(ufact.sym, fungible);

        EVT_ASSERT(fungible.sym == ufact.sym, fungible_symbol_exception, "Symbols are not the same.");

        auto pchecker = make_permission_checker(tokendb);
        if(ufact.issue.valid()) {
            EVT_ASSERT(ufact.issue->name == "issue", permission_type_exception, "Name ${name} does not match with the name of issue permission.", ("name",ufact.issue->name));
            EVT_ASSERT(ufact.issue->threshold > 0 && validate(*ufact.issue), permission_type_exception, "Issue permission is not valid, which may be caused by invalid threshold, duplicated keys or unordered keys.");
            pchecker(*ufact.issue, false);

            fungible.issue = std::move(*ufact.issue);
        }
        if(ufact.manage.valid()) {
            // manage permission's threshold can be 0 which means no one can update permission later.
            EVT_ASSERT(ufact.manage->name == "manage", permission_type_exception, "Name ${name} does not match with the name of manage permission.", ("name",ufact.manage->name));
            EVT_ASSERT(validate(*ufact.manage), permission_type_exception, "Manage permission is not valid, which may be caused by duplicated keys.");
            pchecker(*ufact.manage, false);

            fungible.manage = std::move(*ufact.manage);
        }

        tokendb.update_fungible(fungible);
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

EVT_ACTION_IMPL(issuefungible) {
    using namespace __internal;

    auto ifact = context.act.data_as<issuefungible>();

    try {
        auto sym = ifact.number.get_symbol();
        EVT_ASSERT(context.has_authorized(N128(.fungible), (fungible_name)sym.name()), action_authorize_exception, "Authorized information does not match.");
        EVT_ASSERT(!ifact.address.is_reserved(), fungible_address_exception, "Cannot issue fungible tokens to reserved address");

        auto& tokendb = context.token_db;
        EVT_ASSERT(tokendb.exists_fungible(sym), fungible_exists_exception, "${sym} fungible tokens doesn't exist", ("sym",sym));

        auto addr = get_fungible_address(sym);
        EVT_ASSERT(addr != ifact.address, fungible_address_exception, "From and to are the same address");

        asset from, to;
        tokendb.read_asset(addr, sym, from);
        tokendb.read_asset_no_throw(ifact.address, sym, to);

        EVT_ASSERT(from >= ifact.number, fungible_supply_exception, "Exceeds total supply of ${sym} fungible tokens.", ("sym",sym));

        transfer_fungible(from, to, ifact.number.get_amount());

        tokendb.update_asset(ifact.address, to);
        tokendb.update_asset(addr, from);
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

EVT_ACTION_IMPL(transferft) {
    using namespace __internal;

    auto tfact = context.act.data_as<transferft>();

    try {
        auto sym = tfact.number.get_symbol();
        EVT_ASSERT(context.has_authorized(N128(.fungible), (fungible_name)sym.name()), action_authorize_exception, "Authorized information does not match.");
        EVT_ASSERT(!tfact.to.is_reserved(), fungible_address_exception, "Cannot transfer fungible tokens to reserved address");
        EVT_ASSERT(tfact.from != tfact.to, fungible_address_exception, "From and to are the same address");

        auto& tokendb = context.token_db;
        
        auto facc = asset(0, sym);
        auto tacc = asset(0, sym);
        tokendb.read_asset(tfact.from, sym, facc);
        tokendb.read_asset_no_throw(tfact.to, sym, tacc);

        EVT_ASSERT(facc >= tfact.number, balance_exception, "Address does not have enough balance left.");

        transfer_fungible(facc, tacc, tfact.number.get_amount());

        tokendb.update_asset(tfact.to, tacc);
        tokendb.update_asset(tfact.from, facc);
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

EVT_ACTION_IMPL(evt2pevt) {
    using namespace __internal;

    auto epact = context.act.data_as<evt2pevt>();

    try {
        auto evtsym = epact.number.get_symbol();
        auto pevtsym = symbol(SY(5,PEVT));
        EVT_ASSERT(evtsym == symbol(SY(5,EVT)), fungible_symbol_exception, "Only EVT tokens can be converted to Pinned EVT tokens");
        EVT_ASSERT(context.has_authorized(N128(.fungible), (fungible_name)evtsym.name()), action_authorize_exception, "Authorized information does not match.");
        EVT_ASSERT(!epact.to.is_reserved(), fungible_address_exception, "Cannot convert Pinned EVT tokens to reserved address");

        auto& tokendb = context.token_db;
        
        auto facc = asset(0, evtsym);
        auto tacc = asset(0, pevtsym);
        tokendb.read_asset(epact.from, evtsym, facc);
        tokendb.read_asset_no_throw(epact.to, pevtsym, tacc);

        EVT_ASSERT(facc >= epact.number, balance_exception, "Address does not have enough balance left.");

        transfer_fungible(facc, tacc, epact.number.get_amount());

        tokendb.update_asset(epact.to, tacc);
        tokendb.update_asset(epact.from, facc);
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

namespace __internal {

bool
check_involved_node(const group& group, const group::node& node, const public_key_type& key) {
    auto result = false;
    group.visit_node(node, [&](const auto& n) {
        if(n.is_leaf()) {
            if(group.get_leaf_key(n) == key) {
                result = true;
                // find one, return false to stop iterate group
                return false;
            }
            return true;
        }
        if(check_involved_node(group, n, key)) {
            result = true;
            // find one, return false to stop iterate group
            return false;
        }
        return true;
    });
    return result;
}

auto check_involved_permission = [](const auto& tokendb, const auto& permission, const auto& creator) {
    for(auto& a : permission.authorizers) {
        auto& ref = a.ref;
        switch(ref.type()) {
        case authorizer_ref::account_t: {
            if(creator.is_account_ref() && ref.get_account() == creator.get_account()) {
                return true;
            }
            break;
        }
        case authorizer_ref::group_t: {
            const auto& name = ref.get_group();
            if(creator.is_account_ref()) {
                group_def group;
                tokendb.read_group(name, group);
                if(check_involved_node(group, group.root(), creator.get_account())) {
                    return true;
                }
            }
            else {
                if(name == creator.get_group()) {
                    return true;
                }
            }
        }
        }  // switch
    }
    return false;
};

auto check_involved_domain = [](const auto& tokendb, const auto& domain, auto pname, const auto& creator) {
    switch(pname) {
    case N(issue): {
        return check_involved_permission(tokendb, domain.issue, creator);
    }
    case N(transfer): {
        return check_involved_permission(tokendb, domain.transfer, creator);
    }
    case N(manage): {
        return check_involved_permission(tokendb, domain.manage, creator);
    }
    }  // switch
    return false;
};

auto check_involved_fungible = [](const auto& tokendb, const auto& fungible, auto pname, const auto& creator) {
    switch(pname) {
    case N(manage): {
        return check_involved_permission(tokendb, fungible.manage, creator);
    }
    }  // switch
    return false;
};

auto check_involved_group = [](const auto& group, const auto& key) {
    if(group.key().is_public_key() && group.key().get_public_key() == key) {
        return true;
    }
    return false;
};

auto check_involved_owner = [](const auto& token, const auto& key) {
    for(auto& addr : token.owner) {
        if(addr.is_public_key() && addr.get_public_key() == key) {
            return true;
        }
    }
    return false;
};

template<typename T>
bool
check_duplicate_meta(const T& v, const meta_key& key) {
    if(std::find_if(v.metas.cbegin(), v.metas.cend(), [&](const auto& meta) { return meta.key == key; }) != v.metas.cend()) {
        return true;
    }
    return false;
}

template<>
bool
check_duplicate_meta<group_def>(const group_def& v, const meta_key& key) {
    if(std::find_if(v.metas_.cbegin(), v.metas_.cend(), [&](const auto& meta) { return meta.key == key; }) != v.metas_.cend()) {
        return true;
    }
    return false;  
}

}  // namespace __internal

EVT_ACTION_IMPL(addmeta) {
    using namespace __internal;

    const auto& act   = context.act;
    auto        amact = context.act.data_as<addmeta>();
    try {
        auto& tokendb = context.token_db;

        check_name_reserved(amact.key);

        if(act.domain == N128(.group)) {
            group_def group;
            tokendb.read_group(act.key, group);

            EVT_ASSERT(!check_duplicate_meta(group, amact.key), meta_key_exception, "Metadata with key ${key} already exists.", ("key",amact.key));
            if(amact.creator.is_group_ref()) {
                EVT_ASSERT(amact.creator.get_group() == group.name_, meta_involve_exception, "Only group itself can add its own metadata");
            }
            else {
                // check involved, only group manager(aka. group key) can add meta
                EVT_ASSERT(check_involved_group(group, amact.creator.get_account()), meta_involve_exception, "Creator is not involved in group ${name}.", ("name",act.key));
            }
            group.metas_.emplace_back(meta(amact.key, amact.value, amact.creator));
            tokendb.update_group(group);
        }
        else if(act.domain == N128(.fungible)) {
            fungible_def fungible;
            tokendb.read_fungible(act.key, fungible);

            EVT_ASSERT(!check_duplicate_meta(fungible, amact.key), meta_key_exception, "Metadata with key ${key} already exists.", ("key",amact.key));
            // check involved, only group manager(aka. group key) can add meta
            EVT_ASSERT(check_involved_fungible(tokendb, fungible, N(manage), amact.creator), meta_involve_exception, "Creator is not involved in group ${name}.", ("name",act.key));

            fungible.metas.emplace_back(meta(amact.key, amact.value, amact.creator));
            tokendb.update_fungible(fungible);
        }
        else if(act.key == N128(.meta)) {
            domain_def domain;
            tokendb.read_domain(act.domain, domain);

            EVT_ASSERT(!check_duplicate_meta(domain, amact.key), meta_key_exception, "Metadata with key ${key} already exists.", ("key",amact.key));
            // check involved, only person involved in `manage` permission can add meta
            EVT_ASSERT(check_involved_domain(tokendb, domain, N(manage), amact.creator), meta_involve_exception, "Creator is not involved in domain ${name}.", ("name",act.key));

            domain.metas.emplace_back(meta(amact.key, amact.value, amact.creator));
            tokendb.update_domain(domain);
        }
        else {
            token_def token;
            tokendb.read_token(act.domain, act.key, token);

            EVT_ASSERT(!check_token_destroy(token), token_destoryed_exception, "Token is already destroyed.");
            EVT_ASSERT(!check_duplicate_meta(token, amact.key), meta_key_exception, "Metadata with key ${key} already exists.", ("key",amact.key));

            domain_def domain;
            tokendb.read_domain(act.domain, domain);

            if(amact.creator.is_account_ref()) {
                // check involved, only person involved in `issue` and `transfer` permissions or `owners` can add meta
                auto involved = check_involved_owner(token, amact.creator.get_account())
                    || check_involved_domain(tokendb, domain, N(issue), amact.creator)
                    || check_involved_domain(tokendb, domain, N(transfer), amact.creator);
                EVT_ASSERT(involved, meta_involve_exception, "Creator is not involved in token ${domain}-${name}.", ("domain",act.domain)("name",act.key));
            }
            else {
                // check involved, only group involved in `issue` and `transfer` permissions can add meta
                auto involved = check_involved_domain(tokendb, domain, N(issue), amact.creator)
                    || check_involved_domain(tokendb, domain, N(transfer), amact.creator);
                EVT_ASSERT(involved, meta_involve_exception, "Creator is not involved in token ${domain}-${name}.", ("domain",act.domain)("name",act.key));
            }
            token.metas.emplace_back(meta(amact.key, amact.value, amact.creator));
            tokendb.update_token(token);
        }
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

EVT_ACTION_IMPL(newsuspend) {
    using namespace __internal;

    auto nsact = context.act.data_as<newsuspend>();
    try {
        EVT_ASSERT(context.has_authorized(N128(.suspend), nsact.name), action_authorize_exception, "Authorized information does not match.");

        check_name_reserved(nsact.name);
        for(auto& act : nsact.trx.actions) {
            EVT_ASSERT(act.domain != N128(suspend), suspend_invalid_action_exception, "Actions in 'suspend' domain are not allowd deferred-signning");
        }

        auto& tokendb = context.token_db;
        EVT_ASSERT(!tokendb.exists_suspend(nsact.name), suspend_exists_exception, "Suspend ${name} already exists.", ("name",nsact.name));

        suspend_def suspend;
        suspend.name     = nsact.name;
        suspend.proposer = nsact.proposer;
        suspend.status   = suspend_status::proposed;
        suspend.trx      = std::move(nsact.trx);

        tokendb.add_suspend(suspend);
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

EVT_ACTION_IMPL(aprvsuspend) {
    using namespace __internal;

    auto aeact = context.act.data_as<aprvsuspend>();
    try {
        EVT_ASSERT(context.has_authorized(N128(.suspend), aeact.name), action_authorize_exception, "Authorized information does not match.");

        auto& tokendb = context.token_db;

        suspend_def suspend;
        tokendb.read_suspend(aeact.name, suspend);
        EVT_ASSERT(suspend.status == suspend_status::proposed, suspend_status_exception, "Suspend transaction is not in 'proposed' status.");

        auto signed_keys = suspend.trx.get_signature_keys(aeact.signatures, context.control.get_chain_id());
        auto required_keys = context.control.get_suspend_required_keys(suspend.trx, signed_keys);
        EVT_ASSERT(signed_keys == required_keys, suspend_not_required_keys_exception, "Provided keys are not required in this suspend transaction, provided keys: ${keys}", ("keys",signed_keys));
       
        for(auto it = signed_keys.cbegin(); it != signed_keys.cend(); it++) {
            EVT_ASSERT(suspend.signed_keys.find(*it) == suspend.signed_keys.end(), suspend_duplicate_key_exception, "Public key ${key} is already signed this suspend transaction", ("key",*it)); 
        }

        suspend.signed_keys.merge(signed_keys);
        
        tokendb.update_suspend(suspend);
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

EVT_ACTION_IMPL(cancelsuspend) {
    using namespace __internal;

    auto csact = context.act.data_as<cancelsuspend>();
    try {
        EVT_ASSERT(context.has_authorized(N128(.suspend), csact.name), action_authorize_exception, "Authorized information does not match.");

        auto& tokendb = context.token_db;

        suspend_def suspend;
        tokendb.read_suspend(csact.name, suspend);
        EVT_ASSERT(suspend.status == suspend_status::proposed, suspend_status_exception, "Suspend transaction is not in 'proposed' status.");

        suspend.status = suspend_status::cancelled;
        tokendb.update_suspend(suspend);
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

EVT_ACTION_IMPL(execsuspend) {
    auto esact = context.act.data_as<execsuspend>();
    try {
        EVT_ASSERT(context.has_authorized(N128(.suspend), esact.name), action_authorize_exception, "Authorized information does not match.");

        auto& tokendb = context.token_db;

        suspend_def suspend;
        tokendb.read_suspend(esact.name, suspend);

        EVT_ASSERT(suspend.signed_keys.find(esact.executor) != suspend.signed_keys.end(), suspend_executor_exception, "Executor hasn't sign his key on this suspend transaction");

        auto now = context.control.head_block_time();
        EVT_ASSERT(suspend.status == suspend_status::proposed, suspend_status_exception, "Suspend transaction is not in 'proposed' status.");
        EVT_ASSERT(suspend.trx.expiration > now, suspend_expired_tx_exception, "Suspend transaction is expired at ${expir}, now is ${now}", ("expir",suspend.trx.expiration)("now",now));

        // instead of add signatures to transaction, check authorization and payer here
        context.control.check_authorization(suspend.signed_keys, suspend.trx);
        if(suspend.trx.payer.type() == address::public_key_t) {
            EVT_ASSERT(suspend.signed_keys.find(suspend.trx.payer.get_public_key()) != suspend.signed_keys.end(), payer_exception, "Payer ${pay} needs to sign this suspend transaction", ("pay",suspend.trx.payer));
        }

        auto strx = signed_transaction(suspend.trx, {});
        auto mtrx = std::make_shared<transaction_metadata>(strx);
        auto trace = context.control.push_suspend_transaction(mtrx, fc::time_point::maximum());
        bool transaction_failed = trace && trace->except;
        if(transaction_failed) {
            suspend.status = suspend_status::failed;
            context.console_append(trace->except->to_string());
        }
        else {
            suspend.status = suspend_status::executed;
        }
        tokendb.update_suspend(suspend);
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

EVT_ACTION_IMPL(paycharge) {
    using namespace __internal;
    
    auto pcact = context.act.data_as<const paycharge&>();
    try {
        auto& tokendb = context.token_db;

        auto evt_symbol = symbol(SY(5,EVT));
        auto pevt_symbol = symbol(SY(5,PEVT));

        uint64_t paid = 0;

        asset evt, pevt;
        tokendb.read_asset_no_throw(pcact.payer, pevt_symbol, pevt);
        paid = std::min(pcact.charge, (uint32_t)pevt.get_amount());
        if(paid > 0) {
            pevt -= asset(paid, pevt.get_symbol());
            tokendb.update_asset(pcact.payer, pevt);
        }

        if(paid < pcact.charge) {
            tokendb.read_asset_no_throw(pcact.payer, evt_symbol, evt);
            uint64_t remain = pcact.charge - paid;
            if(evt.get_amount() < remain) {
                EVT_THROW(charge_exceeded_exception, "There are ${e} EVT and ${p} Pinned EVT left, but charge is ${c}", ("e",evt)("p",pevt)("c",pcact.charge));
            }
            evt -= asset(remain, evt.get_symbol());
            tokendb.update_asset(pcact.payer, evt);
        }

        asset evt_asset;
        auto addr = get_fungible_address(evt_symbol);
        tokendb.read_asset(addr, evt_symbol, evt_asset);
        evt_asset += asset(paid, evt_symbol);
    }
    EVT_CAPTURE_AND_RETHROW(tx_apply_exception);
}

}}} // namespace evt::chain::contracts
