/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2017-2026 Christian Schoenebeck                         *
 *                           <cuse@users.sourceforge.net>                  *
 *                                                                         *
 *   This library is part of libgig.                                       *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This library is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this library; if not, write to the Free Software           *
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston,                 *
 *   MA  02111-1307  USA                                                   *
 ***************************************************************************/

#ifndef SRX_FORMAT_H
#define SRX_FORMAT_H

#include "Serialization.h"

namespace Serialization {

/** @brief Encoder / decoder for "Srx" file format.
 *
 * This is the very first file format this library used to encode / decode its
 * serialized data stream. It's a library custom, very simple, unambiguous file
 * format, which does not require any escaping / unescaping or Unicode handling
 * and all their potential culprits involved with those, and therefore provides
 * the benefit of being a trivial, reliable and fast software implementation.
 * Downside on the other hand is that it's not the most user-friendly file
 * format to read or even to edit in encoded form.
 */
class SrxFormat {
public:
    static RawData encode(const Archive& arch);
    static void decode(Archive& arch, const RawData& data);
    static String name();
    static bool checkMagicStart(const RawData& data);
private:
    struct _Blob {
        const char* p;
        const char* end;
    };

    // encoder
    static String _encodeRootBlob(const Archive& arch);
    static String _encodePrimitiveValue(const Object& obj);
    static String _encode(const Object& obj);
    static String _encode(const UID& uid);
    static String _encode(const time_t& time);
    static String _encode(const DataType& type);
    static String _encode(const UIDChain& chain);
    static String _encode(const Member& member);
    static String _encode(const std::vector<Member>& members);
    static String _encode(const Archive::ObjectPool& objects);
    static String _encodeBlob(String data);

    // decoder
    static void _popRootBlob(Archive& arch, const char*& p, const char* end);
    static void _popObjectsBlob(Archive& arch, const char*& p, const char* end);
    static _Blob _decodeBlob(const char* p, const char* end, bool bThrow = true);
    static String _popStringBlob(const char*& p, const char* end);
    static void _popStringBlob(const char*& p, const char* end, RawData& rawData);
    static time_t _popTimeBlob(const char*& p, const char* end);
    static DataType _popDataTypeBlob(const char*& p, const char* end);
    static UID _popUIDBlob(const char*& p, const char* end);
    static UIDChain _popUIDChainBlob(const char*& p, const char* end);
    static Member _popMemberBlob(const char*& p, const char* end);
    static std::vector<Member> _popMembersBlob(const char*& p, const char* end);
    static void _popPrimitiveValue(const char*& p, const char* end, Object& obj);
    static Object _popObjectBlob(const char*& p, const char* end);
    static char _popCharBlob(const char*& p, const char* end);
    static void _popCharBlob(const char*& p, const char* end, RawData& rawData);

    template<typename T_int>
    static T_int _popIntBlob(const char*& p, const char* end);

    template<typename T_int>
    static void _popIntBlob(const char*& p, const char* end, RawData& rawData);

    template<typename T_real>
    static T_real _popRealBlob(const char*& p, const char* end);

    template<typename T_real>
    static void _popRealBlob(const char*& p, const char* end, RawData& rawData);
};

} // namespace Serialization

#endif // SRX_FORMAT_H
