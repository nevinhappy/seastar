/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */
#pragma once

#include <iostream>
#include "core/function_traits.hh"
#include "core/apply.hh"
#include "core/shared_ptr.hh"
#include "core/sstring.hh"
#include "core/future-util.hh"
#include "util/is_smart_ptr.hh"
#include "core/simple-stream.hh"
#include <boost/range/numeric.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include "net/packet-data-source.hh"

namespace rpc {

enum class exception_type : uint32_t {
    USER = 0,
    UNKNOWN_VERB = 1,
};

template<typename T>
struct remove_optional {
    using type = T;
};

template<typename T>
struct remove_optional<optional<T>> {
    using type = T;
};

struct wait_type {}; // opposite of no_wait_type

// tags to tell whether we want a const client_info& parameter
struct do_want_client_info {};
struct dont_want_client_info {};

// An rpc signature, in the form signature<Ret (In0, In1, In2)>.
template <typename Function>
struct signature;

// General case
template <typename Ret, typename... In>
struct signature<Ret (In...)> {
    using ret_type = Ret;
    using arg_types = std::tuple<In...>;
    using clean = signature;
    using want_client_info = dont_want_client_info;
};

// Specialize 'clean' for handlers that receive client_info
template <typename Ret, typename... In>
struct signature<Ret (const client_info&, In...)> {
    using ret_type = Ret;
    using arg_types = std::tuple<In...>;
    using clean = signature<Ret (In...)>;
    using want_client_info = do_want_client_info;
};

template <typename Ret, typename... In>
struct signature<Ret (client_info&, In...)> {
    using ret_type = Ret;
    using arg_types = std::tuple<In...>;
    using clean = signature<Ret (In...)>;
    using want_client_info = do_want_client_info;
};


template <typename T>
struct wait_signature {
    using type = wait_type;
    using cleaned_type = T;
};

template <typename... T>
struct wait_signature<future<T...>> {
    using type = wait_type;
    using cleaned_type = future<T...>;
};

template <>
struct wait_signature<no_wait_type> {
    using type = no_wait_type;
    using cleaned_type = void;
};

template <>
struct wait_signature<future<no_wait_type>> {
    using type = no_wait_type;
    using cleaned_type = future<>;
};

template <typename T>
using wait_signature_t = typename wait_signature<T>::type;

template <typename... In>
inline
std::tuple<In...>
maybe_add_client_info(dont_want_client_info, client_info& ci, std::tuple<In...>&& args) {
    return std::move(args);
}

template <typename... In>
inline
std::tuple<std::reference_wrapper<client_info>, In...>
maybe_add_client_info(do_want_client_info, client_info& ci, std::tuple<In...>&& args) {
    return std::tuple_cat(std::make_tuple(std::ref(ci)), std::move(args));
}

template <bool IsSmartPtr>
struct serialize_helper;

template <>
struct serialize_helper<false> {
    template <typename Serializer, typename Output, typename T>
    static inline void serialize(Serializer& serializer, Output& out, const T& t) {
        return write(serializer, out, t);
    }
};

template <>
struct serialize_helper<true> {
    template <typename Serializer, typename Output, typename T>
    static inline void serialize(Serializer& serializer, Output& out, const T& t) {
        return write(serializer, out, *t);
    }
};

template <typename Serializer, typename Output, typename T>
inline void marshall_one(Serializer& serializer, Output& out, const T& arg) {
    using serialize_helper_type = serialize_helper<is_smart_ptr<typename std::remove_reference<T>::type>::value>;
    serialize_helper_type::serialize(serializer, out, arg);
}

template <typename Serializer, typename Output, typename... T>
inline void do_marshall(Serializer& serializer, Output& out, const T&... args) {
    // C++ guarantees that brace-initialization expressions are evaluted in order
    (void)std::initializer_list<int>{(marshall_one(serializer, out, args), 1)...};
}

template <typename Serializer, typename... T>
inline temporary_buffer<char> marshall(Serializer& serializer, size_t head_space, const T&... args) {
    seastar::measuring_output_stream measure;
    do_marshall(serializer, measure, args...);
    temporary_buffer<char> ret(measure.size() + head_space);
    seastar::simple_output_stream out(ret.get_write(), head_space);
    do_marshall(serializer, out, args...);
    return ret;
}

template <typename Serializer, typename Input>
inline std::tuple<> do_unmarshall(Serializer& serializer, Input& in) {
    return std::make_tuple();
}

template<typename Serializer, typename Input, typename T>
struct unmarshal_one {
    static T doit(Serializer& serializer, Input& in) {
        return read(serializer, in, type<T>());
    }
};

template<typename Serializer, typename Input, typename T>
struct unmarshal_one<Serializer, Input, optional<T>> {
    static optional<T> doit(Serializer& serializer, Input& in) {
        if (in.size()) {
            return optional<T>(read(serializer, in, type<typename remove_optional<T>::type>()));
        } else {
            return optional<T>();
        }
    }
};

template <typename Serializer, typename Input, typename T0, typename... Trest>
inline std::tuple<T0, Trest...> do_unmarshall(Serializer& serializer, Input& in) {
    // FIXME: something less recursive
    auto first = std::make_tuple(unmarshal_one<Serializer, Input, T0>::doit(serializer, in));
    auto rest = do_unmarshall<Serializer, Input, Trest...>(serializer, in);
    return std::tuple_cat(std::move(first), std::move(rest));
}

template <typename Serializer, typename... T>
inline std::tuple<T...> unmarshall(Serializer& serializer, temporary_buffer<char> input) {
    seastar::simple_input_stream in(input.get(), input.size());
    return do_unmarshall<Serializer, seastar::simple_input_stream, T...>(serializer, in);
}

static std::exception_ptr unmarshal_exception(temporary_buffer<char>& data) {
    std::exception_ptr ex;
    auto get = [&data] (size_t size) {
        if (data.size() < size) {
            throw rpc_protocol_error();
        }
        const char *p = data.get();
        data.trim_front(size);
        return p;
    };

    exception_type ex_type = exception_type(read_le<uint32_t>(get(4)));
    uint32_t ex_len = read_le<uint32_t>(get(4));
    switch (ex_type) {
    case exception_type::USER:
        ex = std::make_exception_ptr(std::runtime_error(std::string(get(ex_len), ex_len)));
        break;
    case exception_type::UNKNOWN_VERB:
        ex = std::make_exception_ptr(unknown_verb_error(read_le<uint64_t>(get(8))));
        break;
    default:
        ex = std::make_exception_ptr(unknown_exception_error());
        break;
    }
    return ex;
}

template <typename Payload, typename... T>
struct rcv_reply_base  {
    bool done = false;
    promise<T...> p;
    template<typename... V>
    void set_value(V&&... v) {
        done = true;
        p.set_value(std::forward<V>(v)...);
    }
    ~rcv_reply_base() {
        if (!done) {
            p.set_exception(closed_error());
        }
    }
};

template<typename Serializer, typename MsgType, typename T>
struct rcv_reply : rcv_reply_base<T, T> {
    inline void get_reply(typename protocol<Serializer, MsgType>::client& dst, temporary_buffer<char> input) {
        this->set_value(unmarshall<Serializer, T>(dst.serializer(), std::move(input)));
    }
};

template<typename Serializer, typename MsgType, typename... T>
struct rcv_reply<Serializer, MsgType, future<T...>> : rcv_reply_base<std::tuple<T...>, T...> {
    inline void get_reply(typename protocol<Serializer, MsgType>::client& dst, temporary_buffer<char> input) {
        this->set_value(unmarshall<Serializer, T...>(dst.serializer(), std::move(input)));
    }
};

template<typename Serializer, typename MsgType>
struct rcv_reply<Serializer, MsgType, void> : rcv_reply_base<void, void> {
    inline void get_reply(typename protocol<Serializer, MsgType>::client& dst, temporary_buffer<char> input) {
        this->set_value();
    }
};

template<typename Serializer, typename MsgType>
struct rcv_reply<Serializer, MsgType, future<>> : rcv_reply<Serializer, MsgType, void> {};

template <typename Serializer, typename MsgType, typename Ret, typename... InArgs>
inline auto wait_for_reply(wait_type, std::experimental::optional<steady_clock_type::time_point> timeout, cancellable* cancel, typename protocol<Serializer, MsgType>::client& dst, id_type msg_id,
        signature<Ret (InArgs...)> sig) {
    using reply_type = rcv_reply<Serializer, MsgType, Ret>;
    auto lambda = [] (reply_type& r, typename protocol<Serializer, MsgType>::client& dst, id_type msg_id, temporary_buffer<char> data) mutable {
        if (msg_id >= 0) {
            dst.get_stats_internal().replied++;
            return r.get_reply(dst, std::move(data));
        } else {
            dst.get_stats_internal().exception_received++;
            r.done = true;
            r.p.set_exception(unmarshal_exception(data));
        }
    };
    using handler_type = typename protocol<Serializer, MsgType>::client::template reply_handler<reply_type, decltype(lambda)>;
    auto r = std::make_unique<handler_type>(std::move(lambda));
    auto fut = r->reply.p.get_future();
    dst.wait_for_reply(msg_id, std::move(r), timeout, cancel);
    return fut;
}

template<typename Serializer, typename MsgType, typename... InArgs>
inline auto wait_for_reply(no_wait_type, std::experimental::optional<steady_clock_type::time_point>, cancellable* cancel, typename protocol<Serializer, MsgType>::client& dst, id_type msg_id,
        signature<no_wait_type (InArgs...)> sig) {  // no_wait overload
    return make_ready_future<>();
}

template<typename Serializer, typename MsgType, typename... InArgs>
inline auto wait_for_reply(no_wait_type, std::experimental::optional<steady_clock_type::time_point>, cancellable* cancel, typename protocol<Serializer, MsgType>::client& dst, id_type msg_id,
        signature<future<no_wait_type> (InArgs...)> sig) {  // future<no_wait> overload
    return make_ready_future<>();
}

// Returns lambda that can be used to send rpc messages.
// The lambda gets client connection and rpc parameters as arguments, marshalls them sends
// to a server and waits for a reply. After receiving reply it unmarshalls it and signal completion
// to a caller.
template<typename Serializer, typename MsgType, typename Ret, typename... InArgs>
auto send_helper(MsgType xt, signature<Ret (InArgs...)> xsig) {
    struct shelper {
        MsgType t;
        signature<Ret (InArgs...)> sig;
        auto send(typename protocol<Serializer, MsgType>::client& dst, std::experimental::optional<steady_clock_type::time_point> timeout, cancellable* cancel, const InArgs&... args) {
            if (dst.error()) {
                using cleaned_ret_type = typename wait_signature<Ret>::cleaned_type;
                return futurize<cleaned_ret_type>::make_exception_future(closed_error());
            }

            // send message
            auto msg_id = dst.next_message_id();
            temporary_buffer<char> data = marshall(dst.serializer(), 28, args...);
            auto p = data.get_write() + 8; // 8 extra bytes for expiration timer
            write_le<uint64_t>(p, uint64_t(t));
            write_le<int64_t>(p + 8, msg_id);
            write_le<uint32_t>(p + 16, data.size() - 28);

            // prepare reply handler, if return type is now_wait_type this does nothing, since no reply will be sent
            using wait = wait_signature_t<Ret>;
            return when_all(dst.send(std::move(data), timeout, cancel), wait_for_reply<Serializer, MsgType>(wait(), timeout, cancel, dst, msg_id, sig)).then([] (auto r) {
                    return std::move(std::get<1>(r)); // return future of wait_for_reply
            });
        }
        auto operator()(typename protocol<Serializer, MsgType>::client& dst, const InArgs&... args) {
            return send(dst, {}, nullptr, args...);
        }
        auto operator()(typename protocol<Serializer, MsgType>::client& dst, steady_clock_type::time_point timeout, const InArgs&... args) {
            return send(dst, timeout, nullptr, args...);
        }
        auto operator()(typename protocol<Serializer, MsgType>::client& dst, steady_clock_type::duration timeout, const InArgs&... args) {
            return send(dst, steady_clock_type::now() + timeout, nullptr, args...);
        }
        auto operator()(typename protocol<Serializer, MsgType>::client& dst, cancellable& cancel, const InArgs&... args) {
            return send(dst, {}, &cancel, args...);
        }

    };
    return shelper{xt, xsig};
}

template <typename Serializer, typename MsgType>
inline
future<>
protocol<Serializer, MsgType>::server::connection::respond(int64_t msg_id, temporary_buffer<char>&& data, std::experimental::optional<steady_clock_type::time_point> timeout) {
    auto p = data.get_write();
    write_le<int64_t>(p, msg_id);
    write_le<uint32_t>(p + 8, data.size() - 12);
    return this->send(std::move(data), timeout);
}

template<typename Serializer, typename MsgType, typename... RetTypes>
inline future<> reply(wait_type, future<RetTypes...>&& ret, int64_t msg_id, lw_shared_ptr<typename protocol<Serializer, MsgType>::server::connection> client,
        std::experimental::optional<steady_clock_type::time_point> timeout) {
    if (!client->error()) {
        temporary_buffer<char> data;
        try {
            data = ::apply(marshall<Serializer, const RetTypes&...>,
                    std::tuple_cat(std::make_tuple(std::ref(client->serializer()), 12), std::move(ret.get())));
        } catch (std::exception& ex) {
            uint32_t len = std::strlen(ex.what());
            data = temporary_buffer<char>(20 + len);
            auto p = data.get_write() + 12;
            write_le<uint32_t>(p, uint32_t(exception_type::USER));
            write_le<uint32_t>(p + 4, len);
            std::copy_n(ex.what(), len, p + 8);
            msg_id = -msg_id;
        }

        return client->respond(msg_id, std::move(data), timeout);
    } else {
        return make_ready_future<>();
    }
}

// specialization for no_wait_type which does not send a reply
template<typename Serializer, typename MsgType>
inline future<> reply(no_wait_type, future<no_wait_type>&& r, int64_t msgid, lw_shared_ptr<typename protocol<Serializer, MsgType>::server::connection> client, std::experimental::optional<steady_clock_type::time_point> timeout) {
    try {
        r.get();
    } catch (std::exception& ex) {
        client->get_protocol().log(client->info(), msgid, to_sstring("exception \"") + ex.what() + "\" in no_wait handler ignored");
    }
    return make_ready_future<>();
}

template<typename Ret, typename... InArgs, typename WantClientInfo, typename Func, typename ArgsTuple>
inline futurize_t<Ret> apply(Func& func, client_info& info, WantClientInfo wci, signature<Ret (InArgs...)> sig, ArgsTuple&& args) {
    using futurator = futurize<Ret>;
    try {
        return futurator::apply(func, maybe_add_client_info(wci, info, std::forward<ArgsTuple>(args)));
    } catch (std::runtime_error& ex) {
        return futurator::make_exception_future(std::current_exception());
    }
}

// lref_to_cref is a helper that encapsulates lvalue reference in std::ref() or does nothing otherwise
template<typename T>
auto lref_to_cref(T&& x) {
    return std::move(x);
}

template<typename T>
auto lref_to_cref(T& x) {
    return std::ref(x);
}

// Creates lambda to handle RPC message on a server.
// The lambda unmarshalls all parameters, calls a handler, marshall return values and sends them back to a client
template <typename Serializer, typename MsgType, typename Func, typename Ret, typename... InArgs, typename WantClientInfo>
auto recv_helper(signature<Ret (InArgs...)> sig, Func&& func, WantClientInfo wci) {
    using signature = decltype(sig);
    using wait_style = wait_signature_t<Ret>;
    return [func = lref_to_cref(std::forward<Func>(func))](lw_shared_ptr<typename protocol<Serializer, MsgType>::server::connection> client,
                                                           std::experimental::optional<steady_clock_type::time_point> timeout,
                                                           int64_t msg_id,
                                                           temporary_buffer<char> data) mutable {
        auto memory_consumed = client->estimate_request_size(data.size());
        auto args = unmarshall<Serializer, InArgs...>(client->serializer(), std::move(data));
        // note: apply is executed asynchronously with regards to networking so we cannot chain futures here by doing "return apply()"
        return client->wait_for_resources(memory_consumed).then([client, timeout, msg_id, memory_consumed, args = std::move(args), &func] () mutable {
            try {
                seastar::with_gate(client->get_server().reply_gate(), [client, timeout, msg_id, memory_consumed, args = std::move(args), &func] () mutable {
                    return apply(func, client->info(), WantClientInfo(), signature(), std::move(args)).then_wrapped([client, timeout, msg_id, memory_consumed] (futurize_t<typename signature::ret_type> ret) mutable {
                        return reply<Serializer, MsgType>(wait_style(), std::move(ret), msg_id, client, timeout).finally([client, memory_consumed] {
                            client->release_resources(memory_consumed);
                        });
                    });
                });
            } catch (seastar::gate_closed_exception&) {/* ignore */ }
        });
    };
}

// helper to create copy constructible lambda from non copy constructible one. std::function<> works only with former kind.
template<typename Func>
auto make_copyable_function(Func&& func, std::enable_if_t<!std::is_copy_constructible<std::decay_t<Func>>::value, void*> = nullptr) {
  auto p = make_lw_shared<typename std::decay_t<Func>>(std::forward<Func>(func));
  return [p] (auto&&... args) { return (*p)( std::forward<decltype(args)>(args)... ); };
}

template<typename Func>
auto make_copyable_function(Func&& func, std::enable_if_t<std::is_copy_constructible<std::decay_t<Func>>::value, void*> = nullptr) {
    return std::forward<Func>(func);
}

template<typename Ret, typename... Args>
struct handler_type_helper {
    using type = Ret(Args...);
    static constexpr bool info = false;
};

template<typename Ret, typename First, typename... Args>
struct handler_type_helper<Ret, First, Args...> {
    using type = Ret(First, Args...);
    static constexpr bool info = false;
};

template<typename Ret, typename... Args>
struct handler_type_helper<Ret, const client_info&, Args...> {
    using type = Ret(Args...);
    static constexpr bool info = true;
};

template<typename Ret, typename... Args>
struct handler_type_helper<Ret, client_info&, Args...> {
    using type = Ret(Args...);
    static constexpr bool info = true;
};

template<typename Ret, typename... Args>
struct handler_type_helper<Ret, client_info, Args...> {
    using type = Ret(Args...);
    static constexpr bool info = true;
};

template<typename Ret, typename F, typename I>
struct handler_type_impl;

template<typename Ret, typename F, std::size_t... I>
struct handler_type_impl<Ret, F, std::integer_sequence<std::size_t, I...>> {
    using type = handler_type_helper<Ret, typename remove_optional<typename F::template arg<I>::type>::type...>;
};

// this class is used to calculate client side rpc function signature
// if rpc callback receives client_info as a first parameter it is dropped
// from an argument list and if return type is a smart pointer it is converted to be
// a type it points to, otherwise signature is identical to what was passed to
// make_client().
//
// Examples:
// std::unique_ptr<int>(client_info, int, long) -> int(int, long)
// double(client_info, float) -> double(float)
template<typename Func>
class client_function_type {
    template<typename T, bool IsSmartPtr>
    struct drop_smart_ptr_impl;
    template<typename T>
    struct drop_smart_ptr_impl<T, true> {
        using type = typename T::element_type;
    };
    template<typename T>
    struct drop_smart_ptr_impl<T, false> {
        using type = T;
    };
    template<typename T>
    using drop_smart_ptr = drop_smart_ptr_impl<T, is_smart_ptr<T>::value>;

    using trait = function_traits<Func>;
    // if return type is smart ptr take a type it points to instead
    using return_type = typename drop_smart_ptr<typename trait::return_type>::type;
public:
    using type = typename handler_type_impl<return_type, trait, std::make_index_sequence<trait::arity>>::type::type;
};

template<typename Serializer, typename MsgType>
template<typename Func>
auto protocol<Serializer, MsgType>::make_client(MsgType t) {
    using trait = function_traits<typename client_function_type<Func>::type>;
    using sig_type = signature<typename trait::signature>;
    return send_helper<Serializer>(t, sig_type());
}

template<typename Serializer, typename MsgType>
template<typename Func>
auto protocol<Serializer, MsgType>::register_handler(MsgType t, Func&& func) {
    using sig_type = signature<typename function_traits<Func>::signature>;
    using clean_sig_type = typename sig_type::clean;
    using want_client_info = typename sig_type::want_client_info;
    auto recv = recv_helper<Serializer, MsgType>(clean_sig_type(), std::forward<Func>(func),
            want_client_info());
    register_receiver(t, make_copyable_function(std::move(recv)));
    return make_client<Func>(t);
}

template<typename Serializer, typename MsgType>
protocol<Serializer, MsgType>::server::server(protocol<Serializer, MsgType>& proto, ipv4_addr addr, resource_limits limits)
    : server(proto, engine().listen(addr, listen_options(true)), limits, server_options{})
{}

template<typename Serializer, typename MsgType>
protocol<Serializer, MsgType>::server::server(protocol<Serializer, MsgType>& proto, server_options opts, ipv4_addr addr, resource_limits limits)
    : server(proto, engine().listen(addr, listen_options(true)), limits, opts)
{}

template<typename Serializer, typename MsgType>
protocol<Serializer, MsgType>::server::server(protocol<Serializer, MsgType>& proto, server_socket ss, resource_limits limits, server_options opts)
        : _proto(proto), _ss(std::move(ss)), _limits(limits), _resources_available(limits.max_memory), _options(opts)
{
    accept();
}

template<typename Serializer, typename MsgType>
protocol<Serializer, MsgType>::server::server(protocol<Serializer, MsgType>& proto, server_options opts, server_socket ss, resource_limits limits)
        : server(proto, std::move(ss), limits, opts)
{}

template<typename Serializer, typename MsgType>
void protocol<Serializer, MsgType>::server::accept() {
    keep_doing([this] () mutable {
        return _ss.accept().then([this] (connected_socket fd, socket_address addr) mutable {
            fd.set_nodelay(true);
            auto conn = make_lw_shared<connection>(*this, std::move(fd), std::move(addr), _proto);
            _conns.insert(conn);
            conn->process();
        });
    }).then_wrapped([this] (future<>&& f){
        try {
            f.get();
            assert(false);
        } catch (...) {
            _ss_stopped.set_value();
        }
    });
}

template<typename Serializer, typename MsgType>
protocol<Serializer, MsgType>::server::connection::connection(protocol<Serializer, MsgType>::server& s, connected_socket&& fd, socket_address&& addr, protocol<Serializer, MsgType>& proto)
    : protocol<Serializer, MsgType>::connection(std::move(fd), proto), _server(s) {
    _info.addr = std::move(addr);
}


template<typename Connection>
static void log_exception(Connection& c, const char* log, std::exception_ptr eptr) {
    const char* s;
    try {
        std::rethrow_exception(eptr);
    } catch (std::exception& ex) {
        s = ex.what();
    } catch (...) {
        s = "unknown exception";
    }
    c.get_protocol().log(c.peer_address(), sprint("%s: %s", log, s));
}

template<typename Connection>
static bool verify_frame(Connection& c, temporary_buffer<char>& buf, size_t expected, const char* log) {
    if (buf.size() != expected) {
        if (buf.size() != 0) {
            c.get_protocol().log(c.peer_address(), log);
        }
        return false;
    }
    return true;
}

template<typename Connection>
static future<> send_negotiation_frame(Connection& c, feature_map features) {
    auto negotiation_frame_feature_record_size = [] (const feature_map::value_type& e) {
        return 8 + e.second.size();
    };
    auto extra_len = boost::accumulate(
            features | boost::adaptors::transformed(negotiation_frame_feature_record_size),
            uint32_t(0));
    temporary_buffer<char> reply(sizeof(negotiation_frame) + extra_len);
    auto p = reply.get_write();
    p = std::copy_n(rpc_magic, 8, p);
    write_le<uint32_t>(p, extra_len);
    p += 4;
    for (auto&& e : features) {
        write_le<uint32_t>(p, static_cast<uint32_t>(e.first));
        p += 4;
        write_le<uint32_t>(p, e.second.size());
        p += 4;
        p = std::copy_n(e.second.begin(), e.second.size(), p);
    }
    return c.send_negotiation_frame(std::move(reply));
}

template<typename Connection>
static
future<feature_map>
receive_negotiation_frame(Connection& c, input_stream<char>& in) {
    return in.read_exactly(sizeof(negotiation_frame)).then([&c, &in] (temporary_buffer<char> neg) {
        if (!verify_frame(c, neg, sizeof(negotiation_frame), "unexpected eof during negotiation frame")) {
            return make_exception_future<feature_map>(closed_error());
        }
        negotiation_frame frame;
        std::copy_n(neg.get_write(), sizeof(frame.magic), frame.magic);
        frame.len = read_le<uint32_t>(neg.get_write() + 8);
        if (std::memcmp(frame.magic, rpc_magic, sizeof(frame.magic)) != 0) {
            c.get_protocol().log(c.peer_address(), "wrong protocol magic");
            return make_exception_future<feature_map>(closed_error());
        }
        auto len = frame.len;
        return in.read_exactly(len).then([&c, len] (temporary_buffer<char> extra) {
            if (extra.size() != len) {
                c.get_protocol().log(c.peer_address(), "unexpected eof during negotiation frame");
                return make_exception_future<feature_map>(closed_error());
            }
            feature_map map;
            auto p = extra.get();
            auto end = p + extra.size();
            while (p != end) {
                if (end - p < 8) {
                    c.get_protocol().log(c.peer_address(), "bad feature data format in negotiation frame");
                    return make_exception_future<feature_map>(closed_error());
                }
                auto feature = static_cast<protocol_features>(read_le<uint32_t>(p));
                auto f_len = read_le<uint32_t>(p + 4);
                p += 8;
                if (f_len > end - p) {
                    c.get_protocol().log(c.peer_address(), "buffer underflow in feature data in negotiation frame");
                    return make_exception_future<feature_map>(closed_error());
                }
                auto data = sstring(p, f_len);
                p += f_len;
                map.emplace(feature, std::move(data));
            }
            return make_ready_future<feature_map>(std::move(map));
        });
    });
}

template <typename Serializer, typename MsgType>
template<typename FrameType, typename Info>
typename FrameType::return_type
protocol<Serializer, MsgType>::read_frame(const Info& info, input_stream<char>& in) {
    auto header_size = FrameType::header_size();
    return in.read_exactly(header_size).then([this, header_size, &info, &in] (temporary_buffer<char> header) {
        if (header.size() != header_size) {
            if (header.size() != 0) {
                log(info, sprint("unexpected eof on a %s while reading header: expected %d got %d", FrameType::role(), header_size, header.size()));
            }
            return FrameType::empty_value();
        }
        auto h = FrameType::decode_header(header.get());
        return in.read_exactly(FrameType::get_size(h)).then([this, h = std::move(h), &info] (temporary_buffer<char> data) {
            if (data.size() != FrameType::get_size(h)) {
                log(info, sprint("unexpected eof on a %s while reading data: expected %d got %d", FrameType::role(), FrameType::get_size(h), data.size()));
                return FrameType::empty_value();
            }
            return FrameType::make_value(h, std::move(data));
        });
    });
}

template <typename Serializer, typename MsgType>
template<typename FrameType, typename Info>
typename FrameType::return_type
protocol<Serializer, MsgType>::read_frame_compressed(const Info& info, std::unique_ptr<compressor>& compressor, input_stream<char>& in) {
    if (compressor) {
        return in.read_exactly(4).then([&] (temporary_buffer<char> compress_header) {
            if (compress_header.size() != 4) {
                if (compress_header.size() != 0) {
                    log(info, sprint("unexpected eof on a %s while reading compression header: expected 4 got %d", FrameType::role(), compress_header.size()));
                }
                return FrameType::empty_value();
            }
            auto ptr = compress_header.get();
            auto size = read_le<uint32_t>(ptr);
            return in.read_exactly(size).then([this, size, &compressor, &info] (temporary_buffer<char> compressed_data) {
                if (compressed_data.size() != size) {
                    log(info, sprint("unexpected eof on a %s while reading compressed data: expected %d got %d", FrameType::role(), size, compressed_data.size()));
                    return FrameType::empty_value();
                }
                return do_with(as_input_stream(net::packet(net::packet(), compressor->decompress(std::move(compressed_data)))), [this, &info] (input_stream<char>& in) {
                    return read_frame<FrameType>(info, in);
                });
            });
        });
    } else {
        return read_frame<FrameType>(info, in);
    }
}

template <typename Serializer, typename MsgType>
feature_map
protocol<Serializer, MsgType>::server::connection::negotiate(feature_map requested) {
    feature_map ret;
    for (auto&& e : requested) {
        auto id = e.first;
        switch (id) {
        // supported features go here
        case protocol_features::COMPRESS: {
            if (_server._options.compressor_factory) {
                this->_compressor = _server._options.compressor_factory->negotiate(e.second, true);
                ret[protocol_features::COMPRESS] = _server._options.compressor_factory->supported();
            }
        }
        break;
        case protocol_features::TIMEOUT:
            this->_timeout_negotiated = true;
            ret[protocol_features::TIMEOUT] = "";
            break;
        default:
            // nothing to do
            ;
        }
    }
    return ret;
}

template<typename Serializer, typename MsgType>
future<>
protocol<Serializer, MsgType>::server::connection::negotiate_protocol(input_stream<char>& in) {
    return receive_negotiation_frame(*this, in).then([this, &in] (feature_map requested_features) {
        auto returned_features = negotiate(std::move(requested_features));
        return send_negotiation_frame(*this, std::move(returned_features));
    });
}

template<typename MsgType>
struct request_frame {
    using return_type = future<std::experimental::optional<uint64_t>, MsgType, int64_t, std::experimental::optional<temporary_buffer<char>>>;
    using header_type = std::tuple<std::experimental::optional<uint64_t>, MsgType, int64_t, uint32_t>;
    static size_t header_size() {
        return 20;
    }
    static const char* role() {
        return "server";
    }
    static auto empty_value() {
        return make_ready_future<std::experimental::optional<uint64_t>, MsgType, int64_t, std::experimental::optional<temporary_buffer<char>>>(std::experimental::nullopt, MsgType(0), 0, std::experimental::nullopt);
    }
    static header_type decode_header(const char* ptr) {
        auto type = MsgType(read_le<uint64_t>(ptr));
        auto msgid = read_le<int64_t>(ptr + 8);
        auto size = read_le<uint32_t>(ptr + 16);
        return std::make_tuple(std::experimental::nullopt, type, msgid, size);
    }
    static uint32_t get_size(const header_type& t) {
        return std::get<3>(t);
    }
    static auto make_value(const header_type& t, temporary_buffer<char> data) {
        return make_ready_future<std::experimental::optional<uint64_t>, MsgType, int64_t, std::experimental::optional<temporary_buffer<char>>>(std::get<0>(t), std::get<1>(t), std::get<2>(t), std::move(data));
    }
};

template<typename MsgType>
struct request_frame_with_timeout : request_frame<MsgType> {
    using super = request_frame<MsgType>;
    static size_t header_size() {
        return 28;
    }
    static typename super::header_type decode_header(const char* ptr) {
        auto h = super::decode_header(ptr + 8);
        std::get<0>(h) = read_le<uint64_t>(ptr);
        return h;
    }
};

template <typename Serializer, typename MsgType>
future<std::experimental::optional<uint64_t>, MsgType, int64_t, std::experimental::optional<temporary_buffer<char>>>
protocol<Serializer, MsgType>::server::connection::read_request_frame(input_stream<char>& in) {
    if (this->_timeout_negotiated) {
        return this->_server._proto.template read_frame<request_frame_with_timeout<MsgType>>(_info, in);
    } else {
        return this->_server._proto.template read_frame<request_frame<MsgType>>(_info, in);
    }
}

template <typename Serializer, typename MsgType>
future<std::experimental::optional<uint64_t>, MsgType, int64_t, std::experimental::optional<temporary_buffer<char>>>
protocol<Serializer, MsgType>::server::connection::read_request_frame_compressed(input_stream<char>& in) {
    if (this->_timeout_negotiated) {
        return this->_server._proto.template read_frame_compressed<request_frame_with_timeout<MsgType>>(_info, this->_compressor, in);
    } else {
        return this->_server._proto.template read_frame_compressed<request_frame<MsgType>>(_info, this->_compressor, in);
    }
}

template <typename Serializer, typename MsgType>
void
protocol<Serializer, MsgType>::client::negotiate(feature_map provided) {
    // record features returned here
    for (auto&& e : provided) {
        auto id = e.first;
        switch (id) {
        // supported features go here
        case protocol_features::COMPRESS:
            if (_options.compressor_factory) {
                this->_compressor = _options.compressor_factory->negotiate(e.second, false);
            }
        break;
        case protocol_features::TIMEOUT:
            this->_timeout_negotiated = true;
            break;
        default:
            // nothing to do
            ;
        }
    }
}

template<typename Serializer, typename MsgType>
future<> protocol<Serializer, MsgType>::server::connection::process() {
    send_loop();
    return this->negotiate_protocol(this->_read_buf).then([this] () mutable {
        return do_until([this] { return this->_read_buf.eof() || this->_error; }, [this] () mutable {
            return this->read_request_frame_compressed(this->_read_buf).then([this] (std::experimental::optional<uint64_t> expire, MsgType type, int64_t msg_id, std::experimental::optional<temporary_buffer<char>> data) {
                if (!data) {
                    this->_error = true;
                    return make_ready_future<>();
                } else {
                    std::experimental::optional<steady_clock_type::time_point> timeout;
                    if (expire && *expire) {
                        timeout = steady_clock_type::now() + std::chrono::milliseconds(*expire);
                    }
                    auto it = _server._proto._handlers.find(type);
                    if (it != _server._proto._handlers.end()) {
                        return it->second(this->shared_from_this(), timeout, msg_id, std::move(data.value()));
                    } else {
                        return this->wait_for_resources(28).then([this, timeout, msg_id, type] {
                            // send unknown_verb exception back
                            auto data = temporary_buffer<char>(28);
                            auto p = data.get_write() + 12;
                            write_le<uint32_t>(p, uint32_t(exception_type::UNKNOWN_VERB));
                            write_le<uint32_t>(p + 4, uint32_t(8));
                            write_le<uint64_t>(p + 8, uint64_t(type));
                            try {
                                seastar::with_gate(this->_server._reply_gate, [this, timeout, msg_id, data = std::move(data)] () mutable {
                                    return this->respond(-msg_id, std::move(data), timeout).finally([c = this->shared_from_this()] {
                                        c->release_resources(28);
                                    });
                                });
                            } catch(seastar::gate_closed_exception&) {/* ignore */}
                        });
                    }
                }
            });
        });
    }).then_wrapped([this] (future<> f) {
        if (f.failed()) {
            log_exception(*this, "server connection dropped", f.get_exception());
        }
        this->_error = true;
        return this->stop_send_loop().then_wrapped([this] (future<> f) {
            f.ignore_ready_future();
            this->_server._conns.erase(this->shared_from_this());
            this->_stopped.set_value();
        });
    }).finally([conn_ptr = this->shared_from_this()] {
        // hold onto connection pointer until do_until() exists
    });
}

template<typename Serializer, typename MsgType>
future<>
protocol<Serializer, MsgType>::client::negotiate_protocol(input_stream<char>& in) {
    return receive_negotiation_frame(*this, in).then([this, &in] (feature_map features) {
        return negotiate(features);
    });
}

struct response_frame {
    using return_type = future<int64_t, std::experimental::optional<temporary_buffer<char>>>;
    using header_type = std::tuple<int64_t, uint32_t>;
    static size_t header_size() {
        return 12;
    }
    static const char* role() {
        return "client";
    }
    static auto empty_value() {
        return make_ready_future<int64_t, std::experimental::optional<temporary_buffer<char>>>(0, std::experimental::nullopt);
    }
    static header_type decode_header(const char* ptr) {
        auto msgid = read_le<int64_t>(ptr);
        auto size = read_le<uint32_t>(ptr + 8);
        return std::make_tuple(msgid, size);
    }
    static uint32_t get_size(const header_type& t) {
        return std::get<1>(t);
    }
    static auto make_value(const header_type& t, temporary_buffer<char> data) {
        return make_ready_future<int64_t, std::experimental::optional<temporary_buffer<char>>>(std::get<0>(t), std::move(data));
    }
};

// FIXME: take out-of-line?
template<typename Serializer, typename MsgType>
inline
future<int64_t, std::experimental::optional<temporary_buffer<char>>>
protocol<Serializer, MsgType>::client::read_response_frame(input_stream<char>& in) {
    return this->_proto.template read_frame<response_frame>(this->_server_addr, in);
}

template<typename Serializer, typename MsgType>
inline
future<int64_t, std::experimental::optional<temporary_buffer<char>>>
protocol<Serializer, MsgType>::client::read_response_frame_compressed(input_stream<char>& in) {
    return this->_proto.template read_frame_compressed<response_frame>(this->_server_addr, this->_compressor, in);
}

template<typename Serializer, typename MsgType>
protocol<Serializer, MsgType>::client::client(protocol& proto, client_options ops, seastar::socket socket, ipv4_addr addr, ipv4_addr local)
        : protocol<Serializer, MsgType>::connection(proto), _socket(std::move(socket)), _server_addr(addr), _options(ops) {
    _socket.connect(addr, local).then([this, ops = std::move(ops)] (connected_socket fd) {
        fd.set_nodelay(true);
        if (ops.keepalive) {
            fd.set_keepalive(true);
            fd.set_keepalive_parameters(ops.keepalive.value());
        }
        this->_fd = std::move(fd);
        this->_read_buf = this->_fd.input();
        this->_write_buf = this->_fd.output();
        this->_connected = true;

        feature_map features;
        if (_options.compressor_factory) {
            features[protocol_features::COMPRESS] = _options.compressor_factory->supported();
        }
        if (_options.send_timeout_data) {
            features[protocol_features::TIMEOUT] = "";
        }
        send_negotiation_frame(*this, std::move(features));

        return this->negotiate_protocol(this->_read_buf).then([this] () {
            send_loop();
            return do_until([this] { return this->_read_buf.eof() || this->_error; }, [this] () mutable {
                return this->read_response_frame_compressed(this->_read_buf).then([this] (int64_t msg_id, std::experimental::optional<temporary_buffer<char>> data) {
                    auto it = _outstanding.find(std::abs(msg_id));
                    if (!data) {
                        this->_error = true;
                    } else if (it != _outstanding.end()) {
                        auto handler = std::move(it->second);
                        _outstanding.erase(it);
                        (*handler)(*this, msg_id, std::move(data.value()));
                    } else if (msg_id < 0) {
                        try {
                            std::rethrow_exception(unmarshal_exception(data.value()));
                        } catch(const unknown_verb_error& ex) {
                            // if this is unknown verb exception with unknown id ignore it
                            // can happen if unknown verb was used by no_wait client
                            this->get_protocol().log(this->peer_address(), sprint("unknown verb exception %d ignored", ex.type));
                        } catch(...) {
                            this->_error = true;
                        }
                    } else {
                        // we get a reply for a message id not in _outstanding
                        // this can happened if the message id is timed out already
                        // FIXME: log it but with low level, currently log levels are not supported
                    }
                });
            });
        });
    }).then_wrapped([this] (future<> f) {
        if (f.failed()) {
            log_exception(*this, _connected ? "client connection dropped" : "fail to connect", f.get_exception());
        }
        this->_error = true;
        this->stop_send_loop().then_wrapped([this] (future<> f) {
            f.ignore_ready_future();
            this->_stopped.set_value();
            this->_outstanding.clear();
        });
    });
}

template<typename Serializer, typename MsgType>
protocol<Serializer, MsgType>::client::client(protocol<Serializer, MsgType>& proto, ipv4_addr addr, ipv4_addr local)
    : client(proto, client_options{}, engine().net().socket(), addr, local)
{}

template<typename Serializer, typename MsgType>
protocol<Serializer, MsgType>::client::client(protocol<Serializer, MsgType>& proto, client_options options, ipv4_addr addr, ipv4_addr local)
    : client(proto, options, engine().net().socket(), addr, local)
{}

template<typename Serializer, typename MsgType>
protocol<Serializer, MsgType>::client::client(protocol<Serializer, MsgType>& proto, seastar::socket socket, ipv4_addr addr, ipv4_addr local)
    : client(proto, client_options{}, std::move(socket), addr, local)
{}

}
