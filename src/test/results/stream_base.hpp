//
// Copyright (c) 2016-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

#ifndef BOOST_BEAST_WEBSOCKET_DETAIL_STREAM_BASE_HPP
#define BOOST_BEAST_WEBSOCKET_DETAIL_STREAM_BASE_HPP

#include <boost/beast/core/buffer_size.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/websocket/option.hpp>
#include <boost/beast/websocket/role.hpp>
#include <boost/beast/websocket/detail/frame.hpp>
#include <boost/beast/websocket/detail/prng.hpp>
#include <boost/beast/websocket/detail/pmd_extension.hpp>
#include <boost/beast/websocket/detail/prng.hpp>
#include <boost/beast/zlib/deflate_stream.hpp>
#include <boost/beast/zlib/inflate_stream.hpp>
#include <boost/beast/core/buffers_suffix.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/detail/clamp.hpp>
#include <boost/asio/buffer.hpp>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace boost { @_0_
namespace beast { @_1_
namespace websocket { @_2_
namespace detail { @_3_

//------------------------------------------------------------------------------

template<bool deflateSupported>
struct impl_base;

template<>
struct impl_base<true>
{ @_4_
    // State information for the permessage-deflate extension
    struct pmd_type
    { @_5_
        // `true` if current read message is compressed
        bool rd_set = false;

        zlib::deflate_stream zo;
        zlib::inflate_stream zi;
    }; @_5_

    std::unique_ptr<pmd_type>   pmd_;           // pmd settings or nullptr
    permessage_deflate          pmd_opts_;      // local pmd options
    detail::pmd_offer           pmd_config_;    // offer (client) or negotiation (server)

    // return `true` if current message is deflated
    bool
    rd_deflated() const
    { @_6_
        return pmd_ && pmd_->rd_set;
    } @_6_

    // set whether current message is deflated
    // returns `false` on protocol violation
    bool
    rd_deflated(bool rsv1)
    { @_7_
        if(pmd_)
        {
            pmd_->rd_set = rsv1;
            return true;
        }
        return ! rsv1; // pmd not negotiated
    } @_7_

    // Compress a buffer sequence
    // Returns: `true` if more calls are needed
    //
    template<class ConstBufferSequence>
    bool
    deflate(
        net::mutable_buffer& out,
        buffers_suffix<ConstBufferSequence>& cb,
        bool fin,
        std::size_t& total_in,
        error_code& ec)
    { @_8_
        BOOST_ASSERT(out.size() >= 6);
        auto& zo = this->pmd_->zo;
        zlib::z_params zs;
        zs.avail_in = 0;
        zs.next_in = nullptr;
        zs.avail_out = out.size();
        zs.next_out = out.data();
        for(auto in : beast::buffers_range_ref(cb))
        {
            zs.avail_in = in.size();
            if(zs.avail_in == 0)
                continue;
            zs.next_in = in.data();
            zo.write(zs, zlib::Flush::none, ec);
            if(ec)
            {
                if(ec != zlib::error::need_buffers)
                    return false;
                BOOST_ASSERT(zs.avail_out == 0);
                BOOST_ASSERT(zs.total_out == out.size());
                ec = {};
                break;
            }
            if(zs.avail_out == 0)
            {
                BOOST_ASSERT(zs.total_out == out.size());
                break;
            }
            BOOST_ASSERT(zs.avail_in == 0);
        }
        total_in = zs.total_in;
        cb.consume(zs.total_in);
        if(zs.avail_out > 0 && fin)
        {
            auto const remain = buffer_size(cb);
            if(remain == 0)
            {
                // Inspired by Mark Adler
                // https://github.com/madler/zlib/issues/149
                //
                // VFALCO We could do this flush twice depending
                //        on how much space is in the output.
                zo.write(zs, zlib::Flush::block, ec);
                BOOST_ASSERT(! ec || ec == zlib::error::need_buffers);
                if(ec == zlib::error::need_buffers)
                    ec = {};
                if(ec)
                    return false;
                if(zs.avail_out >= 6)
                {
                    zo.write(zs, zlib::Flush::full, ec);
                    BOOST_ASSERT(! ec);
                    // remove flush marker
                    zs.total_out -= 4;
                    out = net::buffer(out.data(), zs.total_out);
                    return false;
                }
            }
        }
        ec = {};
        out = net::buffer(out.data(), zs.total_out);
        return true;
    } @_8_

    void
    do_context_takeover_write(role_type role)
    { @_9_
        if((role == role_type::client &&
            this->pmd_config_.client_no_context_takeover) ||
           (role == role_type::server &&
            this->pmd_config_.server_no_context_takeover))
        {
            this->pmd_->zo.reset();
        }
    } @_9_

    void
    inflate(
        zlib::z_params& zs,
        zlib::Flush flush,
        error_code& ec)
    { @_10_
        pmd_->zi.write(zs, flush, ec);
    } @_10_

    void
    do_context_takeover_read(role_type role)
    { @_11_
        if((role == role_type::client &&
                pmd_config_.server_no_context_takeover) ||
           (role == role_type::server &&
                pmd_config_.client_no_context_takeover))
        {
            pmd_->zi.clear();
        }
    } @_11_

    template<class Body, class Allocator>
    void
    build_response_pmd(
        http::response<http::string_body>& res,
        http::request<Body,
            http::basic_fields<Allocator>> const& req)
    { @_12_
        pmd_offer offer;
        pmd_offer unused;
        pmd_read(offer, req);
        pmd_negotiate(res, unused, offer, pmd_opts_);
    } @_12_

    void
    on_response_pmd(
        http::response<http::string_body> const& res)
    { @_13_
        detail::pmd_offer offer;
        detail::pmd_read(offer, res);
        // VFALCO see if offer satisfies pmd_config_,
        //        return an error if not.
        pmd_config_ = offer; // overwrite for now
    } @_13_

    template<class Allocator>
    void
    do_pmd_config(
        http::basic_fields<Allocator> const& h)
    { @_14_
        detail::pmd_read(pmd_config_, h);
    } @_14_

    void
    set_option_pmd(permessage_deflate const& o)
    { @_15_
        if( o.server_max_window_bits > 15 ||
            o.server_max_window_bits < 9)
            BOOST_THROW_EXCEPTION(std::invalid_argument{
                "invalid server_max_window_bits"});
        if( o.client_max_window_bits > 15 ||
            o.client_max_window_bits < 9)
            BOOST_THROW_EXCEPTION(std::invalid_argument{
                "invalid client_max_window_bits"});
        if( o.compLevel < 0 ||
            o.compLevel > 9)
            BOOST_THROW_EXCEPTION(std::invalid_argument{
                "invalid compLevel"});
        if( o.memLevel < 1 ||
            o.memLevel > 9)
            BOOST_THROW_EXCEPTION(std::invalid_argument{
                "invalid memLevel"});
        pmd_opts_ = o;
    } @_15_

    void
    get_option_pmd(permessage_deflate& o)
    { @_16_
        o = pmd_opts_;
    } @_16_


    void
    build_request_pmd(http::request<http::empty_body>& req)
    { @_17_
        if(pmd_opts_.client_enable)
        {
            detail::pmd_offer config;
            config.accept = true;
            config.server_max_window_bits =
                pmd_opts_.server_max_window_bits;
            config.client_max_window_bits =
                pmd_opts_.client_max_window_bits;
            config.server_no_context_takeover =
                pmd_opts_.server_no_context_takeover;
            config.client_no_context_takeover =
                pmd_opts_.client_no_context_takeover;
            detail::pmd_write(req, config);
        }
    } @_17_

    void
    open_pmd(role_type role)
    { @_18_
        if(((role == role_type::client &&
                pmd_opts_.client_enable) ||
            (role == role_type::server &&
                pmd_opts_.server_enable)) &&
            pmd_config_.accept)
        {
            detail::pmd_normalize(pmd_config_);
            pmd_.reset(::new pmd_type);
            if(role == role_type::client)
            {
                pmd_->zi.reset(
                    pmd_config_.server_max_window_bits);
                pmd_->zo.reset(
                    pmd_opts_.compLevel,
                    pmd_config_.client_max_window_bits,
                    pmd_opts_.memLevel,
                    zlib::Strategy::normal);
            }
            else
            {
                pmd_->zi.reset(
                    pmd_config_.client_max_window_bits);
                pmd_->zo.reset(
                    pmd_opts_.compLevel,
                    pmd_config_.server_max_window_bits,
                    pmd_opts_.memLevel,
                    zlib::Strategy::normal);
            }
        }
    } @_18_

    void close_pmd()
    { @_19_
        pmd_.reset();
    } @_19_

    bool pmd_enabled() const
    { @_20_
        return pmd_ != nullptr;
    } @_20_

    std::size_t
    read_size_hint_pmd(
        std::size_t initial_size,
        bool rd_done,
        std::uint64_t rd_remain,
        detail::frame_header const& rd_fh) const
    { @_21_
        using beast::detail::clamp;
        std::size_t result;
        BOOST_ASSERT(initial_size > 0);
        if(! pmd_ || (! rd_done && ! pmd_->rd_set))
        {
            // current message is uncompressed

            if(rd_done)
            {
                // first message frame
                result = initial_size;
                goto done;
            }
            else if(rd_fh.fin)
            {
                // last message frame
                BOOST_ASSERT(rd_remain > 0);
                result = clamp(rd_remain);
                goto done;
            }
        }
        result = (std::max)(
            initial_size, clamp(rd_remain));
    done:
        BOOST_ASSERT(result != 0);
        return result;
    } @_21_
}; @_4_

//------------------------------------------------------------------------------

template<>
struct impl_base<false>
{ @_22_
    // These stubs are for avoiding linking in the zlib
    // code when permessage-deflate is not enabled.

    bool
    rd_deflated() const
    { @_23_
        return false;
    } @_23_

    bool
    rd_deflated(bool rsv1)
    { @_24_
        return ! rsv1;
    } @_24_

    template<class ConstBufferSequence>
    bool
    deflate(
        net::mutable_buffer&,
        buffers_suffix<ConstBufferSequence>&,
        bool,
        std::size_t&,
        error_code&)
    { @_25_
        return false;
    } @_25_

    void
    do_context_takeover_write(role_type)
    { @_26_
    } @_26_

    void
    inflate(
        zlib::z_params&,
        zlib::Flush,
        error_code&)
    { @_27_
    } @_27_

    void
    do_context_takeover_read(role_type)
    { @_28_
    } @_28_

    template<class Body, class Allocator>
    void
    build_response_pmd(
        http::response<http::string_body>&,
        http::request<Body,
            http::basic_fields<Allocator>> const&)
    { @_29_
    } @_29_

    void
    on_response_pmd(
        http::response<http::string_body> const&)
    { @_30_
    } @_30_

    template<class Allocator>
    void
    do_pmd_config(http::basic_fields<Allocator> const&)
    { @_31_
    } @_31_

    void
    set_option_pmd(permessage_deflate const& o)
    { @_32_
        if(o.client_enable || o.server_enable)
        {
            // Can't enable permessage-deflate
            // when deflateSupported == false.
            //
            BOOST_THROW_EXCEPTION(std::invalid_argument{
                "deflateSupported == false"});
        }
    } @_32_

    void
    get_option_pmd(permessage_deflate& o)
    { @_33_
        o = {};
        o.client_enable = false;
        o.server_enable = false;
    } @_33_

    void
    build_request_pmd(
        http::request<http::empty_body>&)
    { @_34_
    } @_34_

    void open_pmd(role_type)
    { @_35_
    } @_35_

    void close_pmd()
    { @_36_
    } @_36_

    bool pmd_enabled() const
    { @_37_
        return false;
    } @_37_

    std::size_t
    read_size_hint_pmd(
        std::size_t initial_size,
        bool rd_done,
        std::uint64_t rd_remain,
        frame_header const& rd_fh) const
    { @_38_
        using beast::detail::clamp;
        std::size_t result;
        BOOST_ASSERT(initial_size > 0);
        // compression is not supported
        if(rd_done)
        {
            // first message frame
            result = initial_size;
        }
        else if(rd_fh.fin)
        {
            // last message frame
            BOOST_ASSERT(rd_remain > 0);
            result = clamp(rd_remain);
        }
        else
        {
            result = (std::max)(
                initial_size, clamp(rd_remain));
        }
        BOOST_ASSERT(result != 0);
        return result;
    } @_38_
}; @_22_

//------------------------------------------------------------------------------

struct stream_base
{ @_39_
protected:
    bool secure_prng_ = true;

    std::uint32_t
    create_mask()
    { @_40_
        auto g = make_prng(secure_prng_);
        for(;;)
            if(auto key = g())
                return key;
    } @_40_

}; @_39_

} // detail @_3_
} // websocket @_2_
} // beast @_1_
} // boost @_0_

#endif
