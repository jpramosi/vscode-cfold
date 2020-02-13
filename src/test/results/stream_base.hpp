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
        { @_8_
            pmd_->rd_set = rsv1;
            return true;
        } @_8_
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
    { @_9_
        BOOST_ASSERT(out.size() >= 6);
        auto& zo = this->pmd_->zo;
        zlib::z_params zs;
        zs.avail_in = 0;
        zs.next_in = nullptr;
        zs.avail_out = out.size();
        zs.next_out = out.data();
        for(auto in : beast::buffers_range_ref(cb))
        { @_10_
            zs.avail_in = in.size();
            if(zs.avail_in == 0)
                continue;
            zs.next_in = in.data();
            zo.write(zs, zlib::Flush::none, ec);
            if(ec)
            { @_11_
                if(ec != zlib::error::need_buffers)
                    return false;
                BOOST_ASSERT(zs.avail_out == 0);
                BOOST_ASSERT(zs.total_out == out.size());
                ec = {};
                break;
            } @_11_
            if(zs.avail_out == 0)
            { @_12_
                BOOST_ASSERT(zs.total_out == out.size());
                break;
            } @_12_
            BOOST_ASSERT(zs.avail_in == 0);
        } @_10_
        total_in = zs.total_in;
        cb.consume(zs.total_in);
        if(zs.avail_out > 0 && fin)
        { @_13_
            auto const remain = buffer_size(cb);
            if(remain == 0)
            { @_14_
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
                { @_15_
                    zo.write(zs, zlib::Flush::full, ec);
                    BOOST_ASSERT(! ec);
                    // remove flush marker
                    zs.total_out -= 4;
                    out = net::buffer(out.data(), zs.total_out);
                    return false;
                } @_15_
            } @_14_
        } @_13_
        ec = {};
        out = net::buffer(out.data(), zs.total_out);
        return true;
    } @_9_

    void
    do_context_takeover_write(role_type role)
    { @_16_
        if((role == role_type::client &&
            this->pmd_config_.client_no_context_takeover) ||
           (role == role_type::server &&
            this->pmd_config_.server_no_context_takeover))
        { @_17_
            this->pmd_->zo.reset();
        } @_17_
    } @_16_

    void
    inflate(
        zlib::z_params& zs,
        zlib::Flush flush,
        error_code& ec)
    { @_18_
        pmd_->zi.write(zs, flush, ec);
    } @_18_

    void
    do_context_takeover_read(role_type role)
    { @_19_
        if((role == role_type::client &&
                pmd_config_.server_no_context_takeover) ||
           (role == role_type::server &&
                pmd_config_.client_no_context_takeover))
        { @_20_
            pmd_->zi.clear();
        } @_20_
    } @_19_

    template<class Body, class Allocator>
    void
    build_response_pmd(
        http::response<http::string_body>& res,
        http::request<Body,
            http::basic_fields<Allocator>> const& req)
    { @_21_
        pmd_offer offer;
        pmd_offer unused;
        pmd_read(offer, req);
        pmd_negotiate(res, unused, offer, pmd_opts_);
    } @_21_

    void
    on_response_pmd(
        http::response<http::string_body> const& res)
    { @_22_
        detail::pmd_offer offer;
        detail::pmd_read(offer, res);
        // VFALCO see if offer satisfies pmd_config_,
        //        return an error if not.
        pmd_config_ = offer; // overwrite for now
    } @_22_

    template<class Allocator>
    void
    do_pmd_config(
        http::basic_fields<Allocator> const& h)
    { @_23_
        detail::pmd_read(pmd_config_, h);
    } @_23_

    void
    set_option_pmd(permessage_deflate const& o)
    { @_24_
        if( o.server_max_window_bits > 15 ||
            o.server_max_window_bits < 9)
            BOOST_THROW_EXCEPTION(std::invalid_argument{ @_25_
                "invalid server_max_window_bits"}); @_25_
        if( o.client_max_window_bits > 15 ||
            o.client_max_window_bits < 9)
            BOOST_THROW_EXCEPTION(std::invalid_argument{ @_26_
                "invalid client_max_window_bits"}); @_26_
        if( o.compLevel < 0 ||
            o.compLevel > 9)
            BOOST_THROW_EXCEPTION(std::invalid_argument{ @_27_
                "invalid compLevel"}); @_27_
        if( o.memLevel < 1 ||
            o.memLevel > 9)
            BOOST_THROW_EXCEPTION(std::invalid_argument{ @_28_
                "invalid memLevel"}); @_28_
        pmd_opts_ = o;
    } @_24_

    void
    get_option_pmd(permessage_deflate& o)
    { @_29_
        o = pmd_opts_;
    } @_29_


    void
    build_request_pmd(http::request<http::empty_body>& req)
    { @_30_
        if(pmd_opts_.client_enable)
        { @_31_
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
        } @_31_
    } @_30_

    void
    open_pmd(role_type role)
    { @_32_
        if(((role == role_type::client &&
                pmd_opts_.client_enable) ||
            (role == role_type::server &&
                pmd_opts_.server_enable)) &&
            pmd_config_.accept)
        { @_33_
            detail::pmd_normalize(pmd_config_);
            pmd_.reset(::new pmd_type);
            if(role == role_type::client)
            { @_34_
                pmd_->zi.reset(
                    pmd_config_.server_max_window_bits);
                pmd_->zo.reset(
                    pmd_opts_.compLevel,
                    pmd_config_.client_max_window_bits,
                    pmd_opts_.memLevel,
                    zlib::Strategy::normal);
            } @_34_
            else
            { @_35_
                pmd_->zi.reset(
                    pmd_config_.client_max_window_bits);
                pmd_->zo.reset(
                    pmd_opts_.compLevel,
                    pmd_config_.server_max_window_bits,
                    pmd_opts_.memLevel,
                    zlib::Strategy::normal);
            } @_35_
        } @_33_
    } @_32_

    void close_pmd()
    { @_36_
        pmd_.reset();
    } @_36_

    bool pmd_enabled() const
    { @_37_
        return pmd_ != nullptr;
    } @_37_

    std::size_t
    read_size_hint_pmd(
        std::size_t initial_size,
        bool rd_done,
        std::uint64_t rd_remain,
        detail::frame_header const& rd_fh) const
    { @_38_
        using beast::detail::clamp;
        std::size_t result;
        BOOST_ASSERT(initial_size > 0);
        if(! pmd_ || (! rd_done && ! pmd_->rd_set))
        { @_39_
            // current message is uncompressed

            if(rd_done)
            { @_40_
                // first message frame
                result = initial_size;
                goto done;
            } @_40_
            else if(rd_fh.fin)
            { @_41_
                // last message frame
                BOOST_ASSERT(rd_remain > 0);
                result = clamp(rd_remain);
                goto done;
            } @_41_
        } @_39_
        result = (std::max)(
            initial_size, clamp(rd_remain));
    done:
        BOOST_ASSERT(result != 0);
        return result;
    } @_38_
}; @_4_

//------------------------------------------------------------------------------

template<>
struct impl_base<false>
{ @_42_
    // These stubs are for avoiding linking in the zlib
    // code when permessage-deflate is not enabled.

    bool
    rd_deflated() const
    { @_43_
        return false;
    } @_43_

    bool
    rd_deflated(bool rsv1)
    { @_44_
        return ! rsv1;
    } @_44_

    template<class ConstBufferSequence>
    bool
    deflate(
        net::mutable_buffer&,
        buffers_suffix<ConstBufferSequence>&,
        bool,
        std::size_t&,
        error_code&)
    { @_45_
        return false;
    } @_45_

    void
    do_context_takeover_write(role_type)
    { @_46_
    } @_46_

    void
    inflate(
        zlib::z_params&,
        zlib::Flush,
        error_code&)
    { @_47_
    } @_47_

    void
    do_context_takeover_read(role_type)
    { @_48_
    } @_48_

    template<class Body, class Allocator>
    void
    build_response_pmd(
        http::response<http::string_body>&,
        http::request<Body,
            http::basic_fields<Allocator>> const&)
    { @_49_
    } @_49_

    void
    on_response_pmd(
        http::response<http::string_body> const&)
    { @_50_
    } @_50_

    template<class Allocator>
    void
    do_pmd_config(http::basic_fields<Allocator> const&)
    { @_51_
    } @_51_

    void
    set_option_pmd(permessage_deflate const& o)
    { @_52_
        if(o.client_enable || o.server_enable)
        { @_53_
            // Can't enable permessage-deflate
            // when deflateSupported == false.
            //
            BOOST_THROW_EXCEPTION(std::invalid_argument{ @_54_
                "deflateSupported == false"}); @_54_
        } @_53_
    } @_52_

    void
    get_option_pmd(permessage_deflate& o)
    { @_55_
        o = {};
        o.client_enable = false;
        o.server_enable = false;
    } @_55_

    void
    build_request_pmd(
        http::request<http::empty_body>&)
    { @_56_
    } @_56_

    void open_pmd(role_type)
    { @_57_
    } @_57_

    void close_pmd()
    { @_58_
    } @_58_

    bool pmd_enabled() const
    { @_59_
        return false;
    } @_59_

    std::size_t
    read_size_hint_pmd(
        std::size_t initial_size,
        bool rd_done,
        std::uint64_t rd_remain,
        frame_header const& rd_fh) const
    { @_60_
        using beast::detail::clamp;
        std::size_t result;
        BOOST_ASSERT(initial_size > 0);
        // compression is not supported
        if(rd_done)
        { @_61_
            // first message frame
            result = initial_size;
        } @_61_
        else if(rd_fh.fin)
        { @_62_
            // last message frame
            BOOST_ASSERT(rd_remain > 0);
            result = clamp(rd_remain);
        } @_62_
        else
        { @_63_
            result = (std::max)(
                initial_size, clamp(rd_remain));
        } @_63_
        BOOST_ASSERT(result != 0);
        return result;
    } @_60_
}; @_42_

//------------------------------------------------------------------------------

struct stream_base
{ @_64_
protected:
    bool secure_prng_ = true;

    std::uint32_t
    create_mask()
    { @_65_
        auto g = make_prng(secure_prng_);
        for(;;)
            if(auto key = g())
                return key;
    } @_65_

}; @_64_

} // detail @_3_
} // websocket @_2_
} // beast @_1_
} // boost @_0_

#endif
