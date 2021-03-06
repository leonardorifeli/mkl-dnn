/*******************************************************************************
 * Copyright 2017-2018 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/

#ifndef CPU_WINO_REORDER_HPP
#define CPU_WINO_REORDER_HPP

#include <assert.h>

#include "c_types_map.hpp"
#include "cpu_primitive.hpp"
#include "cpu_reorder_pd.hpp"
#include "type_helpers.hpp"

#include "simple_reorder.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

namespace impl_dtype = mkldnn::impl::data_type;
namespace impl_mfmt = mkldnn::impl::memory_format;

template <impl::data_type_t type>
using data_t = typename prec_traits<type>::type;

template <impl::memory_format_t fmt_i, impl::memory_format_t fmt_o,
        impl::data_type_t type_i, impl::data_type_t type_o>
using enable_if_wino =
        typename utils::enable_if<(fmt_i == goihw || fmt_i == oihw)
                && fmt_o == wino_fmt>::type;

#define WINO_REORDER_TEMPL_DECL                                    \
    impl::data_type_t type_i, impl::memory_format_t fmt_i,         \
            impl::data_type_t type_o, impl::memory_format_t fmt_o, \
            bool order_keep
#define WINO_REORDER_TEMPL_INST type_i, fmt_i, type_o, fmt_o, order_keep

/* high level class declaration */
template <WINO_REORDER_TEMPL_DECL, typename spec = void>
struct wino_reorder_t : public cpu_primitive_t {
    struct pd_t : public cpu_reorder_pd_t {
        pd_t(const cpu_memory_pd_t *input_pd, const cpu_memory_pd_t *output_pd,
                const primitive_attr_t *attr)
            : cpu_reorder_pd_t(input_pd, output_pd, attr) {}

        DECLARE_COMMON_PD_T("wino_reorder", wino_reorder_t);

        static status_t create(reorder_pd_t **reorder_pd,
                const memory_pd_t *input_pd, const memory_pd_t *output_pd,
                const primitive_attr_t *attr) {
            assert(input_pd->engine()->kind() == engine_kind::cpu);
            assert(output_pd->engine()->kind() == engine_kind::cpu);
            const memory_desc_wrapper output_d(output_pd);

            bool args_ok = true
                    && input_pd->desc()->data_type == type_i
                    && output_pd->desc()->data_type == type_o
                    && input_pd->desc()->format == fmt_i
                    && output_pd->desc()->format == fmt_o
                    && output_d.wino_desc().wino_format
                            == mkldnn_wino_wei_aaOIoi;

            if (!args_ok)
                return impl::status::invalid_arguments;

            auto _pd = new pd_t((const cpu_memory_pd_t *)input_pd,
                    (const cpu_memory_pd_t *)output_pd, attr);
            if (_pd == nullptr)
                return out_of_memory;
            if (_pd->init() != success) {
                delete _pd;
                return unimplemented;
            }
            return safe_ptr_assign<reorder_pd_t>(*reorder_pd, _pd);
        }
    };

    typedef typename prec_traits<type_i>::type in_wei_data_t;
    typedef typename prec_traits<type_o>::type out_wei_data_t;

    wino_reorder_t(const pd_t *pd, const input_vector &inputs,
            const output_vector &outputs)
        : cpu_primitive_t(&conf_, inputs, outputs), conf_(*pd) {

        const memory_desc_wrapper input_d(conf_.input_pd());
        const memory_desc_wrapper output_d(conf_.output_pd());

        const int w_alpha = output_d.wino_desc().alpha;

        const auto &in_dims = input_d.dims();
        const auto oc = in_dims[1];
        const auto ic = in_dims[2];

        size_wino_wei_ = w_alpha * w_alpha * oc * ic;
        size_gmgt_ = w_alpha * w_alpha * oc * ic;

        transp_ = (in_wei_data_t *)malloc(
                sizeof(in_wei_data_t) * size_gmgt_, 64);
        wspace_ = (in_wei_data_t *)malloc(
                sizeof(in_wei_data_t) * size_gmgt_, 64);
        tmp_wei_s8_ = (out_wei_data_t *)malloc(
                sizeof(out_wei_data_t) * size_wino_wei_, 64);
        tmp_wei_f32_ = (in_wei_data_t *)malloc(
                sizeof(in_wei_data_t) * size_wino_wei_, 64);
    }

    ~wino_reorder_t() {
        free(transp_);
        free(wspace_);
        free(tmp_wei_f32_);
        free(tmp_wei_s8_);
    }

    enable_if_wino<fmt_i, fmt_o, type_i, type_o> execute_reorder(
            const memory_desc_wrapper &input_d,
            const memory_desc_wrapper &output_d, const data_t<type_i> *input,
            data_t<type_o> *output) {
        const float alpha = conf_.alpha();
        MAYBE_UNUSED(alpha);
        const float beta = conf_.beta();
        MAYBE_UNUSED(beta);

        const auto &in_dims = input_d.dims();

        typedef typename prec_traits<type_i>::type in_wei_data_t;
        typedef typename prec_traits<type_o>::type out_wei_data_t;

        const int r = output_d.wino_desc().r;
        const int w_alpha = output_d.wino_desc().alpha;
        int nb_oc = output_d.wino_desc().nb_oc;
        int nb_ic = output_d.wino_desc().nb_ic;
        int oc_block = output_d.wino_desc().oc_block;
        int ic_block = output_d.wino_desc().ic_block;

        round_mode_t rmode = conf_.attr()->round_mode_;

        int groups;
        int groups_offset;
        if (fmt_i == goihw) {
            groups = in_dims[0];
            groups_offset = 1;
        } else {
            groups = 1;
            groups_offset = 0;
        }
        assert(groups == 1); // groups are not supported now
        MAYBE_UNUSED(groups);

        const auto oc = in_dims[0 + groups_offset];
        const auto ic = in_dims[1 + groups_offset];
        const auto kh = in_dims[2 + groups_offset];
        const auto kw = in_dims[3 + groups_offset];

        auto transp = (in_wei_data_t *)transp_;
        auto wspace = (in_wei_data_t *)wspace_;
        auto tmp_wei_s8 = (out_wei_data_t *)tmp_wei_s8_;
        auto tmp_wei_f32 = (in_wei_data_t *)tmp_wei_f32_;

        utils::array_set((out_wei_data_t *)tmp_wei_s8_, 0, size_wino_wei_);
        utils::array_set((in_wei_data_t *)tmp_wei_f32_, 0, size_wino_wei_);

#pragma omp parallel for collapse(4)
        for (int ioc = 0; ioc < oc; ioc++)
            for (int iic = 0; iic < ic; iic++)
                for (int ih = 0; ih < kh; ih++)
                    for (int iw = 0; iw < kw; iw++)
                        transp[ih * kw * ic * oc + iw * ic * oc + iic * oc
                                + ioc]
                                = input[ioc * ic * kh * kw + iic * kh * kw
                                        + ih * kw + iw];

        /* transform weights to winograd domain */
        float G[4][3] = { { 1.0, 0.0, 0.0 }, { 0.5, 0.5, 0.5 },
            { 0.5, -0.5, 0.5 }, { 0.0, 0.0, 1.0 } };
        const float *g = (float *)G;
        int Z = oc * ic;
        for (int zb = 0; zb < Z / 16; ++zb) {
            auto _inp = transp + zb * 16;
            auto _out = tmp_wei_f32 + zb * 16;

            utils::array_set((float *)wspace, 0, size_gmgt_);
            for (int i = 0; i < r; ++i)
                for (int j = 0; j < w_alpha; ++j)
                    for (int k = 0; k < r; ++k)
                        for (int z = 0; z < 16; ++z)
                            wspace[(i * w_alpha + j) * 16 + z]
                                    += _inp[(i * r + k) * Z + z] * g[j * r + k];

            for (int i = 0; i < w_alpha; ++i)
                for (int j = 0; j < w_alpha; ++j)
                    for (int z = 0; z < 16; ++z) {
                        float t = 0;
                        for (int k = 0; k < r; ++k)
                            t += g[i * r + k]
                                    * wspace[(k * w_alpha + j) * 16 + z];
                        _out[(i * w_alpha + j) * Z + z]
                                = (in_wei_data_t)t; // TODO: saturate
                    }
        }

        /* quantization */
#pragma omp parallel for
        for (int i = 0; i < size_wino_wei_; ++i) {
            tmp_wei_s8[i] = qz_b0<in_wei_data_t, out_wei_data_t>()(
                    tmp_wei_f32[i], alpha, rmode);
        }

        const auto bias_shift = sizeof(out_wei_data_t) * size_wino_wei_;
        const size_t bias_size = w_alpha * w_alpha * oc;

        /* reorder weights in winograd domain*/
        int8_t *s8_output = (int8_t *)(output);
        int32_t *dst_bias = (int32_t *)(output + bias_shift);
        utils::array_set((int32_t *)dst_bias, 0, bias_size);
#pragma omp parallel for collapse(4)
        for (int u_h = 0; u_h < w_alpha; u_h++) {
            for (int u_w = 0; u_w < w_alpha; u_w++) {
                for (int o = 0; o < nb_oc; o++) {
                    for (int ob = 0; ob < oc_block; ob++) {
                        int u_h_shift = u_h * w_alpha * ic * oc;
                        int u_w_shift = u_w * ic * oc;
                        int u_h_shift_b = u_h * w_alpha * oc;
                        int u_w_shift_b = u_w * oc;
                        int oc_block_shift = o * oc_block * ic + ob * ic_block;
                        for (int i = 0; i < nb_ic; i++) {
#pragma omp simd
                            for (int ib = 0; ib < ic_block; ib++) {
                                int _i = i * ic_block;
                                int _o = o * oc_block;
                                int ic_shift = (_i + ib) * oc;
                                int oc_shift = (_o + ob);
                                int ic_block_shift
                                        = i * oc_block * ic_block + ib;
                                int src_offset = u_h_shift + u_w_shift
                                        + ic_shift + oc_shift;
                                int dst_offset = u_h_shift + u_w_shift
                                        + oc_block_shift + ic_block_shift;
                                int bias_offset
                                        = u_h_shift_b + u_w_shift_b + oc_shift;

                                s8_output[dst_offset] = tmp_wei_s8[src_offset];
                                dst_bias[bias_offset]
                                        -= 128 * s8_output[dst_offset];
                            }
                        }
                    }
                }
            }
        }
    }

    virtual void execute(event_t *e) {
        auto input = reinterpret_cast<const data_t<type_i> *>(input_memory(0));
        auto output = reinterpret_cast<data_t<type_o> *>(memory());

        execute_reorder(conf_.input_pd()->desc(), conf_.output_pd()->desc(),
                input, output);

        e->set_state(event_t::ready);
    }

private:
    pd_t conf_;
    void *transp_;
    void *wspace_;
    void *tmp_wei_s8_;
    void *tmp_wei_f32_;
    int size_wino_wei_;
    int size_gmgt_;
};

#undef WINO_REORDER_TEMPL_DECL
#undef WINO_REORDER_TEMPL_INST

} // namespace cpu
} // namespace impl
} // namespace mkldnn

#endif
