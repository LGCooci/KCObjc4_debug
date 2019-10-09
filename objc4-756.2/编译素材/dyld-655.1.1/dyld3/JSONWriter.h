/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include <string.h>

#include <string>
#include <map>
#include <vector>

namespace dyld3 {
namespace json {

struct Node
{
    std::string                 value;
    std::map<std::string, Node> map;
    std::vector<Node>           array;
};

static inline std::string hex(uint64_t value) {
    char buff[64];
    sprintf(buff, "0x%llX", value);
    return buff;
}

static inline std::string hex4(uint64_t value) {
    char buff[64];
    sprintf(buff, "0x%04llX", value);
    return buff;
}

static inline std::string hex8(uint64_t value) {
    char buff[64];
    sprintf(buff, "0x%08llX", value);
    return buff;
}

static inline std::string decimal(uint64_t value) {
    char buff[64];
    sprintf(buff, "%llu", value);
    return buff;
}

static inline void indentBy(uint32_t spaces, FILE* out) {
    for (int i=0; i < spaces; ++i) {
        fprintf(out, " ");
    }
}

static inline void printJSON(const Node& node, uint32_t indent, FILE* out)
{
    if ( !node.map.empty() ) {
        fprintf(out, "{");
        bool needComma = false;
        for (const auto& entry : node.map) {
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n");
            indentBy(indent+2, out);
            fprintf(out, "\"%s\": ", entry.first.c_str());
            printJSON(entry.second, indent+2, out);
            needComma = true;
        }
        fprintf(out, "\n");
        indentBy(indent, out);
        fprintf(out, "}");
    }
    else if ( !node.array.empty() ) {
        fprintf(out, "[");
        bool needComma = false;
        for (const auto& entry : node.array) {
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n");
            indentBy(indent+2, out);
            printJSON(entry, indent+2, out);
            needComma = true;
        }
        fprintf(out, "\n");
        indentBy(indent, out);
        fprintf(out, "]");
    }
    else {
        fprintf(out, "\"%s\"", node.value.c_str());
    }
    if ( indent == 0 )
        fprintf(out, "\n");
}


} // namespace json
} // namespace dyld3
