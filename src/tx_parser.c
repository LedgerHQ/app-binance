/*******************************************************************************
*   (c) 2018 - 2023 Zondax AG
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
********************************************************************************/
#ifdef __cplusplus
#pragma clang diagnostic push
#pragma ide diagnostic ignored "misc-no-recursion"
#endif

#include <jsmn.h>
#include "tx_parser.h"
#include "zxmacros.h"
#include "zxformat.h"
#include "parser_impl.h"

bool extraDepthLevel = false;

// strcat but source does not need to be terminated (a chunk from a bigger string is concatenated)
// dst_max is measured in bytes including the space for NULL termination
// src_size does not include NULL termination
__Z_INLINE void strcat_chunk_s(char *dst, uint16_t dst_max, const char *src_chunk, size_t src_chunk_size) {
    *(dst + dst_max - 1) = 0;                 // last character terminates with zero in case we go beyond bounds
    const size_t prev_size = strlen(dst);

    size_t space_left = dst_max - prev_size - 1;  // -1 because requires termination

    if (src_chunk_size > space_left) {
        src_chunk_size = space_left;
    }

    // Check bounds
    if (src_chunk_size > 0) {
        MEMMOVE(dst + prev_size, src_chunk, src_chunk_size);
        *(dst + prev_size + src_chunk_size) = 0;
    }
}

///////////////////////////
///////////////////////////
///////////////////////////
///////////////////////////
///////////////////////////
///////////////////////////
///////////////////////////

parser_error_t tx_getToken(uint16_t token_index,
                           char *out_val, uint16_t out_val_len,
                           uint8_t pageIdx, uint8_t *pageCount) {
    *pageCount = 0;
    MEMZERO(out_val, out_val_len);

    const int16_t token_start = parser_tx_obj.json.tokens[token_index].start;
    const int16_t token_end = parser_tx_obj.json.tokens[token_index].end;

    if (token_start > token_end) {
        return parser_unexpected_buffer_end;
    }

    const char *inValue = parser_tx_obj.tx + token_start;
    uint16_t inLen = token_end - token_start;

    // empty strings are considered the first page
    *pageCount = 1;
    if (inLen > 0) {
        pageStringExt(out_val, out_val_len, inValue, inLen, pageIdx, pageCount);
    }

    if (pageIdx >= *pageCount) {
        return parser_display_page_out_of_range;
    }

    return parser_ok;
}

__Z_INLINE void append_key_item(uint16_t token_index) {
    if (*parser_tx_obj.query.out_key > 0) {
        // There is already something there, add separator
        strcat_chunk_s(parser_tx_obj.query.out_key,
                       parser_tx_obj.query.out_key_len,
                       "/",
                       1);
    }

    const int16_t token_start = parser_tx_obj.json.tokens[token_index].start;
    const int16_t token_end = parser_tx_obj.json.tokens[token_index].end;
    const char *address_ptr = parser_tx_obj.tx + token_start;
    const int32_t new_item_size = token_end - token_start;

    strcat_chunk_s(parser_tx_obj.query.out_key,
                   parser_tx_obj.query.out_key_len,
                   address_ptr,
                   new_item_size);
}
///////////////////////////
///////////////////////////
///////////////////////////
///////////////////////////
///////////////////////////
///////////////////////////

parser_error_t tx_traverse_find(uint16_t root_token_index, uint16_t *ret_value_token_index) {
    const jsmntype_t token_type = parser_tx_obj.json.tokens[root_token_index].type;

    CHECK_APP_CANARY()

    if (parser_tx_obj.tx == NULL || root_token_index < 0) {
        return parser_no_data;
    }

    if (parser_tx_obj.query.max_level <= 0 || parser_tx_obj.query.max_depth <= 0 ||
        token_type == JSMN_STRING ||
        token_type == JSMN_PRIMITIVE) {
        const bool skipTypeField =
                parser_tx_obj.flags.cache_valid &&
                parser_tx_obj.flags.msg_type_grouping &&
                is_msg_type_field(parser_tx_obj.query.out_key) &&
                parser_tx_obj.filter_msg_type_valid_idx != parser_tx_obj.query._item_index_current;

        const bool skipFromFieldHidingRule =
                parser_tx_obj.flags.msg_from_grouping_hide_all ||
                parser_tx_obj.filter_msg_from_valid_idx != parser_tx_obj.query._item_index_current;

        const bool skipFromField =
                parser_tx_obj.flags.cache_valid &&
                parser_tx_obj.flags.msg_from_grouping &&
                is_msg_from_field(parser_tx_obj.query.out_key) &&
                skipFromFieldHidingRule;

        const bool skipField = skipFromField || skipTypeField;

        CHECK_APP_CANARY()

        // Early bail out
        if (!skipField && parser_tx_obj.query._item_index_current == parser_tx_obj.query.item_index) {
            *ret_value_token_index = root_token_index;
            CHECK_APP_CANARY()
            return parser_ok;
        }

        if (skipField) {
            parser_tx_obj.query.item_index++;
        }

        parser_tx_obj.query._item_index_current++;
        CHECK_APP_CANARY()
        return parser_query_no_results;
    }

    uint16_t el_count;
    parser_error_t err;

    CHECK_PARSER_ERR(object_get_element_count(&parser_tx_obj.json, root_token_index, &el_count))

    switch (token_type) {
        case JSMN_OBJECT: {
            const size_t key_len = strlen(parser_tx_obj.query.out_key);
            for (uint16_t i = 0; i < el_count; ++i) {
                uint16_t key_index;
                uint16_t value_index;

                CHECK_PARSER_ERR(object_get_nth_key(&parser_tx_obj.json, root_token_index, i, &key_index))
                CHECK_PARSER_ERR(object_get_nth_value(&parser_tx_obj.json, root_token_index, i, &value_index))

                // Skip writing keys if we are actually exploring to count
                append_key_item(key_index);
                CHECK_APP_CANARY()

                // When traversing objects both level and depth should be considered
                parser_tx_obj.query.max_level--;
                parser_tx_obj.query.max_depth--;

                // Traverse the value, extracting subkeys
                err = tx_traverse_find(value_index, ret_value_token_index);
                CHECK_APP_CANARY()
                parser_tx_obj.query.max_level++;
                parser_tx_obj.query.max_depth++;

                if (err == parser_ok) {
                    return parser_ok;
                }

                *(parser_tx_obj.query.out_key + key_len) = 0;
                CHECK_APP_CANARY()
            }
            break;
        }
        case JSMN_ARRAY: {
            for (uint16_t i = 0; i < el_count; ++i) {
                uint16_t element_index;
                CHECK_PARSER_ERR(array_get_nth_element(&parser_tx_obj.json,
                                                       root_token_index, i,
                                                       &element_index))
                CHECK_APP_CANARY()

                // When iterating along an array,
                // the level does not change but we need to count the recursion
                parser_tx_obj.query.max_depth--;
                err = tx_traverse_find(element_index, ret_value_token_index);
                parser_tx_obj.query.max_depth++;

                CHECK_APP_CANARY()

                if (err == parser_ok) {
                    return parser_ok;
                }
            }
            break;
        }
        default:
            break;
    }

    return parser_query_no_results;
}

#ifdef __cplusplus
#pragma clang diagnostic pop
#endif
