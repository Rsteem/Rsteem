#include <steemit/chain/account_object.hpp>
#include <steemit/chain/asset_object.hpp>
#include <steemit/chain/database_exceptions.hpp>
#include <steemit/chain/market_object.hpp>
#include <steemit/chain/market_evaluator.hpp>

#include <steemit/chain/database.hpp>
#include <steemit/version/hardfork.hpp>

#include <steemit/protocol/exceptions.hpp>
#include <steemit/protocol/operations/market_operations.hpp>

#include <fc/uint128.hpp>
#include <fc/smart_ref_impl.hpp>

namespace steemit {
    namespace chain {
        /// TODO: after the hardfork, we can rename this method validate_permlink because it is strictily less restrictive than before
        ///  Issue #56 contains the justificiation for allowing any UTF-8 string to serve as a permlink, content will be grouped by tags
        ///  going forwarthis->db.template 
        inline void validate_permlink(const string &permlink) {
            FC_ASSERT(permlink.size() < STEEMIT_MAX_PERMLINK_LENGTH, "permlink is too long");
            FC_ASSERT(fc::is_utf8(permlink), "permlink not formatted in UTF8");
        }

        inline void validate_account_name(const string &name) {
            FC_ASSERT(is_valid_account_name(name), "Account name ${n} is invalid", ("n", name));
        }

        template<uint8_t Major, uint8_t Hardfork, uint16_t Release>
        void convert_evaluator<Major, Hardfork, Release>::do_apply(const operation_type &o) {

            const auto &owner = this->db.template get_account(o.owner);
            FC_ASSERT(this->db.template get_balance(owner, o.amount.symbol) >= o.amount,
                      "Account ${n} does not have sufficient balance for conversion. Balance: ${b}. Required: ${r}",
                      ("n", o.owner)("b", this->db.template get_balance(owner, o.amount.symbol))("r", o.amount));

            this->db.template adjust_balance(owner, -o.amount);

            const auto &fhistory = this->db.template get_feed_history();
            FC_ASSERT(!fhistory.current_median_history.is_null(), "Cannot convert SBD because there is no price feethis->db.template ");

            auto steem_conversion_delay = STEEMIT_CONVERSION_DELAY_PRE_HF16;
            if (this->db.template has_hardfork(STEEMIT_HARDFORK_0_16__551)) {
                steem_conversion_delay = STEEMIT_CONVERSION_DELAY;
            }

            this->db.template create<convert_request_object>([&](convert_request_object &obj) {
                obj.owner = o.owner;
                obj.request_id = o.request_id;
                obj.amount = o.amount;
                obj.conversion_date = this->db.template head_block_time() + steem_conversion_delay;
            });

        }

        template<uint8_t Major, uint8_t Hardfork, uint16_t Release>
        void limit_order_create_evaluator<Major, Hardfork, Release>::do_apply(const operation_type &op) {
            if (this->db.template has_hardfork(STEEMIT_HARDFORK_0_17__115)) {
                try {
                    

                    FC_ASSERT(op.expiration >= this->db.template head_block_time());

                    seller = this->db.template find_account(op.owner);
                    sell_asset = this->db.template find_asset(op.amount_to_sell.symbol);
                    receive_asset = this->db.template find_asset(op.min_to_receive.symbol);

                    if (!sell_asset->options.whitelist_markets.empty()) {
                        FC_ASSERT(sell_asset->options.whitelist_markets.find(receive_asset->asset_name) !=
                                  sell_asset->options.whitelist_markets.end());
                    }
                    if (!sell_asset->options.blacklist_markets.empty()) {
                        FC_ASSERT(sell_asset->options.blacklist_markets.find(receive_asset->asset_name) ==
                                  sell_asset->options.blacklist_markets.end());
                    }

                    FC_ASSERT(this->db.template is_authorized_asset(*seller, *sell_asset));
                    FC_ASSERT(this->db.template is_authorized_asset(*seller, *receive_asset));

                    FC_ASSERT(this->db.template get_balance(*seller, *sell_asset) >= op.amount_to_sell, "insufficient balance",
                              ("balance", this->db.template get_balance(*seller, *sell_asset))("amount_to_sell", op.amount_to_sell));

                } FC_CAPTURE_AND_RETHROW((op))

                try {
                    const auto &seller_stats = this->db.template get_account_statistics(seller->name);
                    this->db.template modify(seller_stats, [&](account_statistics_object &bal) {
                        if (op.amount_to_sell.symbol == STEEM_SYMBOL_NAME) {
                            bal.total_core_in_orders += op.amount_to_sell.amount;
                        }
                    });

                    this->db.template adjust_balance(this->db.template get_account(op.owner), -op.amount_to_sell);

                    bool filled = this->db.template apply_order(this->db.template create<limit_order_object>([&](limit_order_object &obj) {
                        obj.created = this->db.template head_block_time();
                        obj.order_id = op.order_id;
                        obj.seller = seller->name;
                        obj.for_sale = op.amount_to_sell.amount;
                        obj.sell_price = op.get_price();
                        obj.expiration = op.expiration;
                        obj.deferred_fee = deferred_fee;
                    }));

                    FC_ASSERT(!op.fill_or_kill || filled);
                } FC_CAPTURE_AND_RETHROW((op))
            } else {
                FC_ASSERT((op.amount_to_sell.symbol == STEEM_SYMBOL_NAME &&
                           op.min_to_receive.symbol == SBD_SYMBOL_NAME) ||
                          (op.amount_to_sell.symbol == SBD_SYMBOL_NAME && op.min_to_receive.symbol == STEEM_SYMBOL_NAME),
                          "Limit order must be for the STEEM:SBD market");

                FC_ASSERT(op.expiration > this->db.template head_block_time(),
                          "Limit order has to expire after head block time.");

                const auto &owner = this->db.template get_account(op.owner);

                FC_ASSERT(this->db.template get_balance(owner, op.amount_to_sell.symbol) >= op.amount_to_sell,
                          "Account does not have sufficient funds for limit order.");

                this->db.template adjust_balance(owner, -op.amount_to_sell);

                const auto &order = this->db.template create<limit_order_object>([&](limit_order_object &obj) {
                    obj.created = this->db.template head_block_time();
                    obj.seller = op.owner;
                    obj.order_id = op.order_id;
                    obj.for_sale = op.amount_to_sell.amount;
                    obj.sell_price = op.get_price();
                    obj.expiration = op.expiration;
                });

                bool filled = this->db.template apply_order(order);

                if (op.fill_or_kill) {
                    FC_ASSERT(filled, "Cancelling order because it was not fillethis->db.template ");
                }
            }
        }

        template<uint8_t Major, uint8_t Hardfork, uint16_t Release>
        void limit_order_create2_evaluator<Major, Hardfork, Release>::do_apply(const operation_type &op) {
            if (this->db.template has_hardfork(STEEMIT_HARDFORK_0_17__115)) {
                try {
                    

                    FC_ASSERT(op.expiration >= this->db.template head_block_time());

                    seller = this->db.template find_account(op.owner);
                    sell_asset = this->db.template find_asset(op.amount_to_sell.symbol);
                    receive_asset = this->db.template find_asset(op.exchange_rate.quote.symbol);

                    if (!sell_asset->options.whitelist_markets.empty()) {
                        FC_ASSERT(sell_asset->options.whitelist_markets.find(receive_asset->asset_name) !=
                                  sell_asset->options.whitelist_markets.end());
                    }
                    if (!sell_asset->options.blacklist_markets.empty()) {
                        FC_ASSERT(sell_asset->options.blacklist_markets.find(receive_asset->asset_name) ==
                                  sell_asset->options.blacklist_markets.end());
                    }

                    FC_ASSERT(this->db.template is_authorized_asset(*seller, *sell_asset));
                    FC_ASSERT(this->db.template is_authorized_asset(*seller, *receive_asset));

                    FC_ASSERT(this->db.template get_balance(*seller, *sell_asset) >= op.amount_to_sell, "insufficient balance",
                              ("balance", this->db.template get_balance(*seller, *sell_asset))("amount_to_sell", op.amount_to_sell));

                } FC_CAPTURE_AND_RETHROW((op))

                try {
                    const auto &seller_stats = this->db.template get_account_statistics(seller->name);
                    this->db.template modify(seller_stats, [&](account_statistics_object &bal) {
                        if (op.amount_to_sell.symbol == STEEM_SYMBOL_NAME) {
                            bal.total_core_in_orders += op.amount_to_sell.amount;
                        }
                    });

                    this->db.template adjust_balance(this->db.template get_account(op.owner), -op.amount_to_sell);

                    bool filled = this->db.template apply_order(this->db.template create<limit_order_object>([&](limit_order_object &obj) {
                        obj.created = this->db.template head_block_time();
                        obj.order_id = op.order_id;
                        obj.seller = seller->name;
                        obj.for_sale = op.amount_to_sell.amount;
                        obj.sell_price = op.get_price();
                        obj.expiration = op.expiration;
                        obj.deferred_fee = deferred_fee;
                    }));

                    FC_ASSERT(!op.fill_or_kill || filled);
                } FC_CAPTURE_AND_RETHROW((op))
            } else {
                FC_ASSERT((op.amount_to_sell.symbol == STEEM_SYMBOL_NAME &&
                           op.exchange_rate.quote.symbol == SBD_SYMBOL_NAME) ||
                          (op.amount_to_sell.symbol == SBD_SYMBOL_NAME &&
                           op.exchange_rate.quote.symbol == STEEM_SYMBOL_NAME),
                          "Limit order must be for the STEEM:SBD market");

                FC_ASSERT(op.expiration > this->db.template head_block_time(),
                          "Limit order has to expire after head block time.");

                const auto &owner = this->db.template get_account(op.owner);

                FC_ASSERT(this->db.template get_balance(owner, op.amount_to_sell.symbol) >= op.amount_to_sell,
                          "Account does not have sufficient funds for limit order.");

                this->db.template adjust_balance(owner, -op.amount_to_sell);

                const auto &order = this->db.template create<limit_order_object>([&](limit_order_object &obj) {
                    obj.created = this->db.template head_block_time();
                    obj.seller = op.owner;
                    obj.order_id = op.order_id;
                    obj.for_sale = op.amount_to_sell.amount;
                    obj.sell_price = op.exchange_rate;
                    obj.expiration = op.expiration;
                });

                bool filled = this->db.template apply_order(order);

                if (op.fill_or_kill) {
                    FC_ASSERT(filled, "Cancelling order because it was not fillethis->db.template ");
                }
            }
        }

        template<uint8_t Major, uint8_t Hardfork, uint16_t Release>
        void limit_order_cancel_evaluator<Major, Hardfork, Release>::do_apply(const operation_type &op) {
            if (this->db.template has_hardfork(STEEMIT_HARDFORK_0_17__115)) {
                try {
                    _order = this->db.template find_limit_order(op.owner, op.order_id);
                    FC_ASSERT(_order->seller == op.owner);
                } FC_CAPTURE_AND_RETHROW((op))

                try {
                    auto base_asset = _order->sell_price.base.symbol;
                    auto quote_asset = _order->sell_price.quote.symbol;

                    this->db.template cancel_order(*_order, false /* don't create a virtual op*/);

                    // Possible optimization: order can be called by canceling a limit order iff the canceled order was at the top of the book.
                    // Do I need to check calls in both assets?
                    this->db.template check_call_orders(this->db.template get_asset(base_asset));
                    this->db.template check_call_orders(this->db.template get_asset(quote_asset));
                } FC_CAPTURE_AND_RETHROW((op))
            } else {
                this->db.template cancel_order(this->db.template get_limit_order(op.owner, op.order_id), false);
            }
        }

        template<uint8_t Major, uint8_t Hardfork, uint16_t Release>
        void call_order_update_evaluator<Major, Hardfork, Release>::do_apply(const operation_type &op) {
            try {
                _paying_account = this->db.template find_account(op.funding_account);
                _debt_asset = this->db.template find_asset(op.delta_debt.symbol);

                FC_ASSERT(_debt_asset->is_market_issued(),
                          "Unable to cover ${sym} as it is not a collateralized asset.",
                          ("sym", _debt_asset->asset_name));

                _bitasset_data = this->db.template find_asset_bitasset_data(_debt_asset->asset_name);

                /// if there is a settlement for this asset, then no further margin positions may be taken and
                /// all existing margin positions should have been closed va database::globally_settle_asset
                FC_ASSERT(!_bitasset_data->has_settlement());

                FC_ASSERT(op.delta_collateral.symbol == _bitasset_data->options.short_backing_asset);

                if (_bitasset_data->is_prediction_market) {
                    FC_ASSERT(op.delta_collateral.amount == op.delta_debt.amount);
                } else if (_bitasset_data->current_feethis->db.template settlement_price.is_null()) {
                    FC_THROW_EXCEPTION(insufficient_feeds, "Cannot borrow asset with no price feethis->db.template ");
                }

                if (op.delta_debt.amount < 0) {
                    FC_ASSERT(this->db.template get_balance(*_paying_account, *_debt_asset) >= op.delta_debt,
                              "Cannot cover by ${c} when payer only has ${b}",
                              ("c", op.delta_debt.amount)("b", this->db.template get_balance(*_paying_account, *_debt_asset).amount));
                }

                if (op.delta_collateral.amount > 0) {
                    FC_ASSERT(
                            this->db.template get_balance(*_paying_account, this->db.template get_asset(_bitasset_data->options.short_backing_asset)) >=
                            op.delta_collateral, "Cannot increase collateral by ${c} when payer only has ${b}",
                            ("c", op.delta_collateral.amount)("b", this->db.template get_balance(*_paying_account, this->db.template get_asset(op.delta_collateral.symbol)).amount));
                }
            } FC_CAPTURE_AND_RETHROW((op))

            try {
                

                if (op.delta_debt.amount != 0) {
                    this->db.template adjust_balance(this->db.template get_account(op.funding_account), op.delta_debt);

                    // Deduct the debt paid from the total supply of the debt asset.
                    this->db.template modify(this->db.template get_asset_dynamic_data(_debt_asset->asset_name),
                             [&](asset_dynamic_data_object &dynamic_asset) {
                                 dynamic_asset.current_supply += op.delta_debt.amount;
                                 assert(dynamic_asset.current_supply >= 0);
                             });
                }

                if (op.delta_collateral.amount != 0) {
                    this->db.template adjust_balance(this->db.template get_account(op.funding_account), -op.delta_collateral);

                    // Adjust the total core in orders accodingly
                    if (op.delta_collateral.symbol == STEEM_SYMBOL_NAME) {
                        this->db.template modify(this->db.template get_account_statistics(_paying_account->name),
                                 [&](account_statistics_object &stats) {
                                     stats.total_core_in_orders += op.delta_collateral.amount;
                                 });
                    }
                }


                auto &call_idx = this->db.template get_index<call_order_index>().indices().template get<by_account>();
                auto itr = call_idx.find(boost::make_tuple(op.funding_account, op.delta_debt.symbol));
                const call_order_object *call_obj = nullptr;

                if (itr == call_idx.end()) {
                    FC_ASSERT(op.delta_collateral.amount > 0);
                    FC_ASSERT(op.delta_debt.amount > 0);

                    call_obj = &this->db.template create<call_order_object>([&](call_order_object &call) {
                        call.order_id = op.order_id;
                        call.borrower = op.funding_account;
                        call.collateral = op.delta_collateral.amount;
                        call.debt = op.delta_debt.amount;
                        call.call_price = price::call_price(op.delta_debt, op.delta_collateral,
                                                            _bitasset_data->current_feethis->db.template maintenance_collateral_ratio);

                    });
                } else {
                    call_obj = &*itr;

                    this->db.template modify(*call_obj, [&](call_order_object &call) {
                        call.collateral += op.delta_collateral.amount;
                        call.debt += op.delta_debt.amount;
                        if (call.debt > 0) {
                            call.call_price = price::call_price(call.get_debt(), call.get_collateral(),
                                                                _bitasset_data->current_feethis->db.template maintenance_collateral_ratio);
                        }
                    });
                }

                auto debt = call_obj->get_debt();
                if (debt.amount == 0) {
                    FC_ASSERT(call_obj->collateral == 0);
                    this->db.template remove(*call_obj);
                    return void();
                }

                FC_ASSERT(call_obj->collateral > 0 && call_obj->debt > 0);

                // then we must check for margin calls and other issues
                if (!_bitasset_data->is_prediction_market) {
                    call_order_object::id_type call_order_id = call_obj->id;

                    // check to see if the order needs to be margin called now, but don't allow black swans and require there to be
                    // limit orders available that could be used to fill the order.
                    if (this->db.template check_call_orders(*_debt_asset, false)) {
                        const auto order_obj = this->db.template find<call_order_object, by_id>(call_order_id);
                        // if we filled at least one call order, we are OK if we totally fillethis->db.template 
                        STEEMIT_ASSERT(!order_obj, call_order_update_unfilled_margin_call,
                                       "Updating call order would trigger a margin call that cannot be fully filled",
                                       ("a", ~order_obj->call_price)("b",
                                                                     _bitasset_data->current_feethis->db.template settlement_price));
                    } else {
                        const auto order_obj = this->db.template find<call_order_object, by_id>(call_order_id);
                        FC_ASSERT(order_obj, "no margin call was executed and yet the call object was deleted");
                        //edump( (~order_obj->call_price) ("<")( _bitasset_data->current_feethis->db.template settlement_price) );
                        // We didn't fill any call orders.  This may be because we
                        // aren't in margin call territory, or it may be because there
                        // were no matching orders.  In the latter case, we throw.
                        STEEMIT_ASSERT(~order_obj->call_price < _bitasset_data->current_feethis->db.template settlement_price,
                                       call_order_update_unfilled_margin_call,
                                       "Updating call order would trigger a margin call that cannot be fully filled",
                                       ("a", ~order_obj->call_price)("b",
                                                                     _bitasset_data->current_feethis->db.template settlement_price));
                    }
                }
            } FC_CAPTURE_AND_RETHROW((op))
        }

        template<uint8_t Major, uint8_t Hardfork, uint16_t Release>
        void bid_collateral_evaluator<Major, Hardfork, Release>::do_apply(const operation_type &o) {
            const account_object &paying_account = this->db.template get_account(o.bidder);

            try {
                const asset_object &debt_asset = this->db.template get_asset(o.debt_coverethis->db.template symbol);
                FC_ASSERT(debt_asset.is_market_issued(),
                          "Unable to cover ${sym} as it is not a collateralized asset.",
                          ("sym", debt_asset.asset_name));

                const asset_bitasset_data_object &bitasset_data = this->db.template get_asset_bitasset_data(debt_asset.asset_name);

                FC_ASSERT(bitasset_data.has_settlement());

                FC_ASSERT(o.additional_collateral.symbol == bitasset_data.options.short_backing_asset);

                FC_ASSERT(!bitasset_data.is_prediction_market, "Cannot bid on a prediction market!");

                if (o.additional_collateral.amount > 0) {
                    FC_ASSERT(this->db.template get_balance(paying_account, this->db.template get_asset_bitasset_data(
                            bitasset_data.options.short_backing_asset).asset_name) >= o.additional_collateral,
                              "Cannot bid ${c} collateral when payer only has ${b}",
                              ("c", o.additional_collateral.amount)("b", this->db.template get_balance(paying_account, this->db.template get_asset(
                                      o.additional_collateral.symbol)).amount));
                }

                const auto &index = this->db.template get_index<collateral_bid_index>().indices().template get<by_account>();
                const auto &bid = index.find(boost::make_tuple(o.debt_coverethis->db.template symbol, o.bidder));
                if (bid != index.end()) {
                    _bid = &(*bid);
                } else
                    FC_ASSERT(o.debt_coverethis->db.template amount > 0, "Can't find bid to cancel?!");
            } FC_CAPTURE_AND_RETHROW((o))


            try {
                if (_bid != nullptr) {
                    this->db.template cancel_bid(*_bid, false);
                }

                if (o.debt_coverethis->db.template amount == 0) {
                    return;
                }

                this->db.template adjust_balance(paying_account, -o.additional_collateral);
                _bid = &this->db.template create<collateral_bid_object>([&](collateral_bid_object &bid) {
                    bithis->db.template bidder = o.bidder;
                    bithis->db.template inv_swan_price = o.additional_collateral / o.debt_covered;
                });
            } FC_CAPTURE_AND_RETHROW((o))
        }
    }
}