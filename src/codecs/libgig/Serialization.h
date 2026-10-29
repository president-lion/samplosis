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

#ifndef LIBGIG_SERIALIZATION_H
#define LIBGIG_SERIALIZATION_H

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <typeinfo>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <time.h>
#include <stdarg.h>
#include <assert.h>
#include <functional>
#include <sstream>
#include <locale>
#include <limits>

#include "sysdef.h"

#ifndef __has_extension
# define __has_extension(x) 0
#endif

#ifndef HAS_BUILTIN_TYPE_TRAITS
# if __cplusplus >= 201103L
#  define HAS_BUILTIN_TYPE_TRAITS 1
# elif ( __has_extension(is_class) && __has_extension(is_enum) )
#  define HAS_BUILTIN_TYPE_TRAITS 1
# elif ( __GNUC__ > 4 || ( __GNUC__ == 4 && __GNUC_MINOR__ >= 3 ) )
#  define HAS_BUILTIN_TYPE_TRAITS 1
# elif _MSC_VER >= 1400 /* MS Visual C++ 8.0 (Visual Studio 2005) */
#  define HAS_BUILTIN_TYPE_TRAITS 1
# elif __INTEL_COMPILER >= 1100
#  define HAS_BUILTIN_TYPE_TRAITS 1
# else
#  define HAS_BUILTIN_TYPE_TRAITS 0
# endif
#endif

#if !HAS_BUILTIN_TYPE_TRAITS
# include <tr1/type_traits>
# define LIBGIG_IS_CLASS(type) std::tr1::__is_union_or_class<type>::value //NOTE: without compiler support we cannot distinguish union from class
#else
# define LIBGIG_IS_CLASS(type) __is_class(type)
#endif

/** @brief C++ Serialization / Deserialization Framework.
 *
 * The classes in this namespace allow to serialize and deserialize <b>native
 * C++ objects</b> in a portable, very easy and yet flexible way.
 *
 * See class Archive as starting point for how to implement serialization and
 * deserialization with your C++ application.
 *
 * Serialization is a technique that allows to capture the <b>current state and
 * data</b> of native (in this case C++) objects and @b encoding that overall
 * state into a <b>serial data stream</b>, including all other objects the
 * "serialized" objects relate to. The data stream may then be sent over
 * "wire", for example as file or via network connection to another computer,
 * and finally the data stream would be "deserialized" on receiver side, that
 * is <b>fully automatic resembling of objects' original state and data</b> as
 * it was captured on sender side.
 *
 * Main features of this framework:
 *
 * - <b>Very low time & effort integration</b>:
 *   you only need to define @b "what" shall be (de)serialized of your own C++
 *   classes, not @b "how". Which is as simple as this:
 *   @code
 *   void MyOwnClass::serialize(Serialization::Archive* archive) {
 *     SRLZ(memberVar1);
 *     SRLZ(memberVar2);
 *     SRLZ(memberVar3);
 *     ...
 *   }
 *   @endcode
 *
 * - <b>Standard types out of the box</b>:
 *   fundamental <b>data types</b> and <b>standard STL classes</b> are handled
 *   <b>out of the box</b>. So you don't need to specify how to serialize a
 *   @c std::map<std::string,int> container or even a simple
 *   @c double variable for instance.
 *
 * - <b>No mandatory tree-structure layout of your C++ classes</b>:
 *   it copes with @b cyclic @b dependencies (e.g. a C++ object pointing at
 *   another C++ object and vice versa) without causing endless recursion or
 *   data redundancy.
 *
 * - <b>Automatic C++ pointer handling</b>:
 *   raw C++ pointers are handled fully automatically, i.e. automatically
 *   identifies and distinguishes @b "strong" from @b "weak" @b pointers,
 *   automatically <b>creates required instances</b> for <b>fundamental
 *   types</b> and <b>arbitrary C++ objects</b> for strong pointers on receiver
 *   side (without redundancy or dead links), and therefore fully automatically
 *   resembles even very complex C++ object layouts of applications from
 *   serialized data streams.
 *
 * - <b>Supports "partial" or "masked" deserialization</b>:
 *   application may decide to deserialize <b>only certain objects</b> or
 *   even <b>only certain members</b> of <b>individual objects</b>, leaving all
 *   other existing data of live C++ objects untouched, therefore allowing for
 *   instance to implement a flexible and powerful preset or macro feature in
 *   applications very easily and with very little effort
 *   (see
 *   <a href="http://doc.linuxsampler.org/Release_Notes/LinuxSampler_2_1_0/#macros">
 *   Gigedit's macro feature as example</a>).
 *
 * - <b>Multiple encoding formats</b>:
 *   the encoding format of the serialized data stream can be chosen. For
 *   instance a standard @b JSON text encoding is supported
 *   (see Archive::format_t).
 *
 * - <b>Reflection API</b>:
 *   Native C++ objects can be modified <b>live & directly</b> via this
 *   framework's reflection API.
 *
 * - <b>Portable (de)serialization</b>:
 *   sender side (serializing application) and receiver side (deserializing
 *   application) may run on completely different computer systems, with
 *   different hardware, OS, CPU architecture, native memory word size and
 *   endian type. This is handled automatically.
 *
 * - <b>Long-term robustness on application changes</b>:
 *   automatically handles @b changes between your application's <b>software
 *   versions</b>. If sender and receiver are using different versions of their
 *   serialized / deserialized C++ classes, structures and data types, that is
 *   having a <b>different, changed native data structure layout</b>, this
 *   framework automatically adapts its deserialization process in that case,
 *   such that deserialized objects on receiver side would still reflect the
 *   overall expected states and overall data as intended by the sender, with
 *   the new native data structure layout expected by receiver side. For being
 *   able to do so, this framework automatically captures all kind of
 *   additional information about each serialized object and each data
 *   structure member, for example name of each data structure member and
 *   native offset within its containing data structure, precise data types,
 *   and much more.
 */
namespace Serialization {

    // just symbol prototyping
    class DataType;
    class Object;
    class Member;
    class Archive;
    class Exception;

    /** @brief Textual string.
     *
     * This type is used for built-in automatic serialization / deserialization
     * of C++ @c String objects (a.k.a. @c std::string from the STL). This
     * framework supports serializing this common data type out of the box and
     * is handled by this framework as it was a primitive C++ data type.
     */
    typedef std::string String;

    /** @brief Array<> template.
     *
     * This type is used for built-in automatic serialization / deserialization
     * of C++ array containers (a.k.a. @c std::vector from the STL). This
     * framework supports serializing this common data type out of the box, with
     * only one constraint: the precise element type used with arrays must be
     * serializable. So the array's element type should either be a) any
     * primitive data type (e.g. @c int, @c double, etc.) or b) any other data
     * structure or class types enjoying out of the box serialization support by
     * this framework, or c) if it is a custom @c struct or @c class then it
     * must have a @c serialize() method implementation.
     */
    template<class T>
    using Array = std::vector<T>;

    /** @brief Set<> template.
     *
     * This type is used for built-in automatic serialization / deserialization
     * of C++ unique data set containers (a.k.a. @c std::set from the STL). This
     * framework supports serializing this common data type out of the box, with
     * the following constraint: the precise key type used with sets must be
     * either a primitive data type (e.g. @c int, @c double, @c bool, etc.) or a
     * @c String object.
     */
    template<class T>
    using Set = std::set<T>;

    /** @brief Map<> template.
     *
     * This type is used for built-in automatic serialization / deserialization
     * of C++ associative sorted map containers (a.k.a. @c std::map from the
     * STL). This framework supports serializing this common data type out of
     * the box, with the following 2 constraints:
     *
     * 1. The precise key type (i.e. 1st template parameter) used with maps must
     *    be either a primitive data type (e.g. @c int, @c double, @c bool,
     *    etc.) or a @c String object.
     *
     * 2. The value type (i.e. 2nd template parameter) must be serializable. So
     *    the map's value type should either be a) any primitive data type (e.g.
     *    @c int, @c double, etc.) or b) any other data structure or class types
     *    enjoying out of the box serialization support by this framework, or c)
     *    if it is a custom @c struct or @c class then it must have a
     *    @c serialize() method implementation.
     */
    template<class T_key, class T_value>
    using Map = std::map<T_key,T_value>;

    /** @brief Raw data stream of serialized C++ objects.
     *
     * This data type is used for the data stream as a result of serializing
     * your C++ objects with Archive::serialize(), and for native raw data
     * representation of individual serialized C/C++ objects, members and variables.
     *
     * @see Archive::rawData(), Object::rawData()
     */
    typedef std::vector<uint8_t> RawData;

    /** @brief Abstract identifier for serialized C++ objects.
     *
     * This data type is used for identifying serialized C++ objects and members
     * of your C++ objects. It is important to know that such an ID might not
     * necessarily be unique. For example the ID of one C++ object might often
     * be identical to the ID of the first member of that particular C++ object.
     * That's why there is additionally the concept of an UID in this framework.
     *
     * @see UID
     */
    typedef void* ID;

    /** @brief Version number data type.
     *
     * This data type is used for maintaining version number information of
     * your C++ class implementations.
     *
     * @see Archive::setVersion() and Archive::setMinVersion()
     */
    typedef uint32_t Version;

    /** @brief To which time zone a certain timing information relates to.
     *
     * The constants in this enum type are used to define to which precise time
     * zone a time stamp relates to.
     */
    enum time_base_t {
        LOCAL_TIME, ///< The time stamp relates to the machine's local time zone. Request a time stamp in local time if you want to present that time stamp to the end user.
        UTC_TIME ///< The time stamp relates to "Greenwhich Mean Time" zone, also known as "Coordinated Universal Time". Request time stamp with UTC if you want to compare that time stamp with other time stamps.
    };

    /** @brief Check whether data is a C/C++ @c enum type.
     *
     * Returns true if the supplied C++ variable or object is of a C/C++ @c enum
     * type.
     *
     * @param data - the variable or object whose data type shall be checked
     */
    template<typename T>
    bool IsEnum(const T& /*data*/) {
        #if !HAS_BUILTIN_TYPE_TRAITS
        return std::tr1::is_enum<T>::value;
        #else
        return __is_enum(T);
        #endif
    }

    /** @brief Check whether data is a C++ @c union type.
     *
     * Returns true if the supplied C++ variable or object is of a C/C++ @c union
     * type. Note that the result of this function is only reliable if the C++
     * compiler you are using has support for built-in type traits. If your C++
     * compiler does not have built-in type traits support, then this function
     * will simply return @c false on all your calls.
     *
     * @param data - the variable or object whose data type shall be checked
     */
    template<typename T>
    bool IsUnion(const T& /*data*/) {
        #if !HAS_BUILTIN_TYPE_TRAITS
        return false; // without compiler support we cannot distinguish union from class
        #else
        return __is_union(T);
        #endif
    }

    /** @brief Check whether data is a C/C++ @c struct or C++ @c class type.
     *
     * Returns true if the supplied C++ variable or object is of C/C++ @c struct
     * or C++ @c class type. Note that if you are using a C++ compiler which
     * does have built-in type traits support, then this function will also
     * return @c true on C/C++ @c union types.
     *
     * @param data - the variable or object whose data type shall be checked
     */
    template<typename T>
    bool IsClass(const T& /*data*/) {
        #if !HAS_BUILTIN_TYPE_TRAITS
        return std::tr1::__is_union_or_class<T>::value; // without compiler support we cannot distinguish union from class
        #else
        return __is_class(T);
        #endif
    }

    /*template<typename T>
    bool IsTrivial(T data) {
        return __is_trivial(T);
    }*/

    /*template<typename T>
    bool IsPOD(T data) {
        return __is_pod(T);
    }*/

    /*template<typename T>
    bool IsArray(const T& data) {
        return false;
    }*/

    /*template<typename T>
    bool IsArray(const Array<T>& data) {
        return true;
    }*/

    // generalized C-locale guaranteed T to string conversion
    template<typename T> inline
    String toString(const T& value) {
        //TODO: replace by locale-independent, maybe faster std::to_chars() [C++17]
        std::stringstream ss;
        ss.imbue(std::locale::classic()); // a.k.a. "C" locale
        ss << value;
        return ss.str();
    }

    // C-locale guaranteed double to string conversion
    template<> inline
    String toString(const double& value) {
        //TODO: replace by locale-independent, maybe faster std::to_chars() [C++17]
        std::stringstream ss;
        ss.imbue(std::locale::classic()); // a.k.a. "C" locale
        // stringstream has lower precision by default compared to std::to_string()
        ss.precision(std::numeric_limits<double>::max_digits10);
        ss << value;
        return ss.str();
    }

    template<typename T> inline
    String toString(T* ptr) {
        std::stringstream ss;
        ss.imbue(std::locale::classic()); // a.k.a. "C" locale
        ss << "0x" << std::hex << size_t(ptr);
        return ss.str();
    }

    template<> inline
    String toString(const String& value) {
        return value;
    }

#if LIBGIG_SERIALIZATION_INTERNAL
    // prototyping of private internal friend functions
    template<typename T>
    static T _primitiveObjectValueToNumber(const Object& obj);
#endif // LIBGIG_SERIALIZATION_INTERNAL

    /** @brief Abstract reflection of a native C++ data type.
     *
     * Provides detailed information about a serialized C++ data type, whether
     * it is a fundamental C/C++ data type (like @c int, @c float, @c char,
     * etc.) or custom defined data types like a C++ @c class, C/C++ @c struct,
     * @c enum, as well as other features of the respective data type like its
     * native memory size and more.
     *
     * All informations provided by this class are retrieved from the
     * respective individual C++ objects, their members and other data when
     * they are serialized, and all those information are stored with the
     * serialized archive and its resulting data stream. Due to the availability
     * of these extensive data type information within serialized archives, this
     * framework is capable to use them in order to adapt its deserialization
     * process upon subsequent changes to your individual C++ classes.
     */
    class DataType {
    public:
        DataType();
        size_t size() const { return m_size; } ///< Returns native memory size of the respective C++ object or variable.
        bool isValid() const;
        bool isPointer() const;
        bool isClass() const;
        bool isPrimitive() const;
        bool isString() const;
        bool isChar() const;
        bool isInteger() const;
        bool isReal() const;
        bool isNumber() const;
        bool isBool() const;
        bool isEnum() const;
        bool isArray() const;
        bool isSet() const;
        bool isMap() const;
        bool isSigned() const;
        operator bool() const { return isValid(); } ///< Same as calling isValid().
        //bool operator()() const { return isValid(); }
        bool operator==(const DataType& other) const;
        bool operator!=(const DataType& other) const;
        bool operator<(const DataType& other) const;
        bool operator>(const DataType& other) const;
        String asLongDescr() const;
        String baseTypeName() const;
        String customTypeName(bool demangle = false) const;
        String customTypeName2(bool demangle = false) const;

        /** @brief Construct a DataType object for the given native C++ data.
         *
         * Use this function to create corresponding DataType objects for
         * native C/C++ objects, members and variables. Run-time type
         * information (RTTI) is used to resolve its type information.
         *
         * @param data - native C/C++ object/member/variable a DataType object
         *               shall be created for
         * @param registerType - wether type information shall be registered
         *                       (default: yes)
         * @returns corresponding DataType object for the supplied native C/C++
         *          object/member/variable
         * @see dataType() for compile-time known data types
         * @see registerNativeDataType() for registration of type information
         */
        template<typename T>
        static DataType dataTypeOf(const T& data, bool registerType = true) {
            const DataType type = Resolver<T>::resolve(data);
            if (registerType)
                registerNativeDataType(type, data);
            return type;
        }

        /** @brief Construct a DataType object for the given native C++ type.
         *
         * This methods basically behaves similar to above's method, whith the
         * difference that above's method takes an argument to real native data
         * and resolves its run-time type information (RTTI), whereas this
         * version here just takes a compile-time constant template argument.
         *
         * @param registerType - wether type information shall be registered
         *                       (default: yes)
         * @returns corresponding DataType object for the supplied C/C++ type
         * @see dataTypeOf() for run-time variant data types
         * @see registerNativeDataType() for registration of type information
         */
        template<typename T>
        static DataType dataType(bool registerType = true) {
            T unused = T();
            const DataType type = Resolver<T>::resolve(unused);
            if (registerType)
                registerNativeDataType(type, unused);
            return type;
        }

        /** @brief True size of passed native data in bytes.
         *
         * In most cases the result is equal to C++ sizeof(data). However for
         * polymomorphic objects being referenced as (some) base pointer, the
         * C++ sizeof(data) operator would potentially return a value that's
         * smaller than the actual size of the referenced object. In such kind
         * of scenarios this method instead returns the correct, actual (larger)
         * size of the referenced object.
         */
        template<typename T>
        static size_t sizeOf(const T& data) {
            DataType type = dataTypeOf(data);
            const auto itNativeType = m_nativeTypes.find(type.internalID());
            return (itNativeType != m_nativeTypes.end()) ?
                   itNativeType->second.size : sizeof(data);
        }

        /** @brief Manual registration of native C++ data types.
         *
         * This method allows applications to explicitly register native C/C++
         * data types to this framework, for the purpose to allow creating new
         * instances of those data types by this framework's reflection API at
         * runtime.
         *
         * @discussion Mostly it is not necessary to manually register native
         * C/C++ data types, as this framework automatically collects required
         * information about applications' data types at compile-time. There are
         * rare cases though where an application needs to register some native
         * data types explicitly to this Serialization framework by either
         * calling @c DataType::registerNativeType() or by using class
         * @c NativeDataTypeRegistry, before doing any (de)serialization.
         *
         * For instance: if the application serializes only base class pointers
         * and all of the sudden some of these pointers point to objects of
         * derived classes @b AND this framework did not get into touch with
         * those derived class(es) elsewhere, in this example the deriving
         * classe(s) either might need to be registered manually, or some member
         * variable of that derived type needs to be serialized somewhere for
         * this framework to gather the required type information at compile
         * time instead.
         *
         * Example:
         *
         * @code
         * void appsInitCode() {
         *    Serialization::DataType::registerNativeType<MyType>();
         * }
         * @endcode
         *
         * @see NativeDataTypeRegistry for registration at global C++
         *      declaration space
         */
        template<typename T>
        static void registerNativeDataType();

        /** @brief Registrator for native data types.
         *
         * Explicitly registers some of application's native C/C++ data types
         * at C++ global declaration space.
         *
         * This is just a wrapper class for calling the static method
         * DataType::registerNativeDataType<T>(), as functions in general cannot
         * be called directly from a global declaration space in C++.
         *
         * Example:
         *
         * @code
         * // e.g. placed at the beginning of app's source file 'MyType.cpp'
         * static Serialization::DataType::NativeDataTypeRegistry<MyType> _registerMyType;
         * @endcode
         *
         * You might define a convenience macro in your app like this:
         *
         * @code
         * // e.g. placed in some of app's shared header file
         * #define REGISTER_TYPE(type) \
         *     static Serialization::DataType::NativeDataTypeRegistry<type> _register_##type
         *
         * // e.g. placed at the beginning of app's source file 'MyType.cpp'
         * REGISTER_TYPE(MyType);
         * @endcode
         *
         * @see registerNativeDataType() for discussion about manual registration
         */
        template<typename T>
        class NativeDataTypeRegistry {
        public:
            NativeDataTypeRegistry() {
                DataType::registerNativeDataType<T>();
            }
        };

    protected:
        DataType(bool isPointer, int size, String baseType,
                 String customType1 = "", String customType2 = "");

        String internalID() const;
        Object newInstance(Archive* archive) const;

        template<typename T, typename std::enable_if<
                 !std::is_pointer<T>::value &&
                 std::is_default_constructible<T>::value, bool>::type = true>
        static void registerNativeDataType(const DataType& type, const T& nativeData);

        template<typename T, typename std::enable_if<
                 !std::is_pointer<T>::value &&
                 !std::is_default_constructible<T>::value, bool>::type = true>
        static void registerNativeDataType(const DataType& type, const T& nativeData);

        template<typename T, typename std::enable_if<
                 std::is_default_constructible<T>::value, bool>::type = true>
        static void registerNativeDataType(const DataType& type, const T* const& nativeData);

        template<typename T, typename std::enable_if<
                 !std::is_default_constructible<T>::value, bool>::type = true>
        static void registerNativeDataType(const DataType& type, const T* const& nativeData);

        // DataType resolver for primitive / built-in types
        template<typename T, bool T_isPointer>
        struct ResolverBase {
            static DataType resolve(const T& data);
        };

        // DataType resolver for non-pointer types
        template<typename T>
        struct Resolver : ResolverBase<T,false> {
            static DataType resolve(const T& data) {
                return ResolverBase<T,false>::resolve(data);
            }
        };

        // DataType resolver for pointer types (of 1st degree)
        template<typename T>
        struct Resolver<T*> : ResolverBase<T,true> {
            static DataType resolve(const T* const & data) {
                return ResolverBase<T,true>::resolve(*data);
            }
        };

        // DataType resolver for non-pointer Array<> container object types.
        template<typename T>
        struct Resolver<Array<T>> {
            static DataType resolve(const Array<T>& data) {
                const int sz = sizeof(data);
                T unused = T();
                return DataType(false, sz, "Array", rawCppTypeNameOf(unused));
            }
        };

        // DataType resolver for Array<> pointer types (of 1st degree).
        template<typename T>
        struct Resolver<Array<T>*> {
            static DataType resolve(const Array<T>*& data) {
                const int sz = sizeof(*data);
                T unused = T();
                return DataType(true, sz, "Array", rawCppTypeNameOf(unused));
            }
        };

        // DataType resolver for non-pointer Set<> container object types.
        template<typename T>
        struct Resolver<Set<T>> {
            static DataType resolve(const Set<T>& data) {
                const int sz = sizeof(data);
                T unused = T();
                return DataType(false, sz, "Set", rawCppTypeNameOf(unused));
            }
        };

        // DataType resolver for Set<> pointer types (of 1st degree).
        template<typename T>
        struct Resolver<Set<T>*> {
            static DataType resolve(const Set<T>*& data) {
                const int sz = sizeof(*data);
                T unused = T();
                return DataType(true, sz, "Set", rawCppTypeNameOf(unused));
            }
        };

        // DataType resolver for non-pointer Map<> container object types.
        template<typename T_key, typename T_value>
        struct Resolver<Map<T_key,T_value>> {
            static DataType resolve(const Map<T_key,T_value>& data) {
                const int sz = sizeof(data);
                T_key unused1 = T_key();
                T_value unused2 = T_value();
                return DataType(false, sz, "Map", rawCppTypeNameOf(unused1),
                                rawCppTypeNameOf(unused2));
            }
        };

        // DataType resolver for Map<> pointer types (of 1st degree).
        template<typename T_key, typename T_value>
        struct Resolver<Map<T_key,T_value>*> {
            static DataType resolve(const Map<T_key,T_value>*& data) {
                const int sz = sizeof(*data);
                T_key unused1 = T_key();
                T_value unused2 = T_value();
                return DataType(true, sz, "Map", rawCppTypeNameOf(unused1),
                                rawCppTypeNameOf(unused2));
            }
        };

        // for compile-time known types
        template<typename T>
        static String rawCppTypeName() {
            const std::type_info& type = typeid(T);
            #if defined _MSC_VER // Microsoft compiler ...
            String name = type.raw_name();
            #else // i.e. especially GCC and clang ...
            String name = type.name();
            #endif
            //while (!name.empty() && name[0] >= 0 && name[0] <= 9)
            //    name = name.substr(1);
            return name;
        }

        // for RTTI resolved pointer types
        template<typename T>
        static String rawCppTypeNameOf(const T* const & data) {
            return rawCppTypeName<T*>();
        }

        // for RTTI resolved non-pointer types
        template<typename T>
        static String rawCppTypeNameOf(const T& data) {
            const std::type_info& type = typeid(data);
            #if defined _MSC_VER // Microsoft compiler ...
            String name = type.raw_name();
            #else // i.e. especially GCC and clang ...
            String name = type.name();
            #endif
            //while (!name.empty() && name[0] >= 0 && name[0] <= 9)
            //    name = name.substr(1);
            return name;
        }

    private:
        struct NativeType {
            size_t size;
            std::function<Object(Archive*)> allocFn;
        };

        int m_size;
        bool m_isPointer;
        String m_baseTypeName;
        String m_customTypeName;
        String m_customTypeName2;

        static std::map<String,NativeType> m_nativeTypes;

        friend class Archive;
        friend class SrxFormat;
        friend class SrxJSONDecoder;
    };

    /** @brief Unique identifier referring to one specific native C++ object, member, fundamental variable, or any other native C++ data.
     *
     * Reflects a unique identifier for one specific serialized C++ data, i.e.
     * C++ class instance, C/C++ struct instance, member, primitive pointer,
     * fundamental variables, or any other native C/C++ data originally being
     * serialized.
     *
     * A unique identifier is composed of an id (an identifier which is not
     * necessarily unique) and a size. Since the underlying ID is derived from
     * the original C++ object's memory location, such an ID is not sufficient
     * to distinguish a particular C++ object from the first member of that C++
     * object, since both typically share the same memory address. So
     * additionally the memory size of the respective object or member is
     * bundled with UID objects to make them unique and distinguishable.
     */
    class UID {
    public:
        ID id; ///< Abstract non-unique ID of the object or member in question.
        size_t size; ///< Memory size of the object or member in question.

        bool isValid() const;
        operator bool() const { return isValid(); } ///< Same as calling isValid().
        //bool operator()() const { return isValid(); }
        bool operator==(const UID& other) const { return id == other.id && size == other.size; }
        bool operator!=(const UID& other) const { return id != other.id || size != other.size; }
        bool operator<(const UID& other) const { return id < other.id || (id == other.id && size < other.size); }
        bool operator>(const UID& other) const { return id > other.id || (id == other.id && size > other.size); }

        /** @brief Create an unique indentifier for a native C++ object/member/variable.
         *
         * Creates and returns an unique identifier for the passed native C++
         * object, object member or variable. For the same C++ object/member/variable
         * this function will always return the same UID. For all other ones,
         * this function is guaranteed to return a different UID.
         */
        template<typename T>
        static UID from(const T& obj) {
            return Resolver<T>::resolve(obj);
        }

    protected:
        // UID resolver for non-pointer types
        template<typename T>
        struct Resolver {
            static UID resolve(const T& obj);
        };

        // UID resolver for pointer types (of 1st degree)
        template<typename T>
        struct Resolver<T*> {
            static UID resolve(const T* const & obj);
        };
    };

    /**
     * Reflects an invalid UID and behaves similar to NULL as invalid value for
     * pointer types. All UID objects are first initialized with this value,
     * and it essentially an all zero object.
     */
    extern const UID NO_UID;

    /** @brief Chain of UIDs.
     *
     * This data type is used for native C++ pointers. The first member of the
     * UID chain is the unique identifier of the C++ pointer itself, then the
     * following UIDs are the respective objects or variables the pointer is
     * pointing to. The size (the amount of elements) of the UIDChain depends
     * solely on the degree of the pointer type. For example the following C/C++
     * pointer:
     * @code
     * int* pNumber;
     * @endcode
     * is an integer pointer of first degree. Such a pointer would have a
     * UIDChain with 2 members: the first element would be the UID of the
     * pointer itself, the second element of the chain would be the integer data
     * that pointer is pointing to. In the following example:
     * @code
     * bool*** pppSomeFlag;
     * @endcode
     * That boolean pointer would be of third degree, and thus its UIDChain
     * would have a size of 4 (elements).
     *
     * Accordingly a non pointer type like:
     * @code
     * float f;
     * @endcode
     * would yield in a UIDChain of size 1.
     *
     * Since however this serialization framework currently only supports
     * pointers of first degree yet, all UIDChains are currently either of
     * size 1 or 2, which might change in future though.
     */
    typedef std::vector<UID> UIDChain;

    /** @brief Abstract reflection of a native C++ class/struct's member variable.
     *
     * Provides detailed information about a specific C++ member variable of
     * serialized C++ object, like its C++ data type, offset of this member
     * within its containing data structure/class, its C++ member variable name
     * and more.
     *
     * Consider you defined the following user defined C/C++ @c struct type in
     * your application:
     * @code
     * struct Foo {
     *     int  a;
     *     bool b;
     *     double someValue;
     * };
     * @endcode
     * Then @c a, @c b and @c someValue are "members" of @c struct @c Foo for
     * instance. So that @c struct would have 3 members in the latter example.
     *
     * @see Object::members()
     */
    class Member {
    public:
        Member();
        UID uid() const;
        UID parentUID() const;
        String name() const;
        ssize_t offset() const;
        const DataType& type() const;
        bool isValid() const;
        operator bool() const { return isValid(); } ///< Same as calling isValid().
        //bool operator()() const { return isValid(); }
        bool operator==(const Member& other) const;
        bool operator!=(const Member& other) const;
        bool operator<(const Member& other) const;
        bool operator>(const Member& other) const;

    protected:
        Member(String name, UID uid, ssize_t offset, DataType type, const Object& parent);
        friend class Archive;

    private:
        UID m_uid;
        ssize_t m_offset;
        String m_name;
        DataType m_type;
        UID m_parentUID;

        friend class SrxFormat;
        friend class SrxJSONDecoder;
    };

    /** @brief Abstract reflection of some native serialized C/C++ data.
     *
     * When your native C++ objects are serialized, all native data is
     * translated and reflected by such an Object reflection. So each instance
     * of your serialized native C++ class objects become available as an
     * Object, but also each member variable of your C++ objects is translated
     * into an Object, and any other native C/C++ data. So essentially every
     * native data is turned into its own Object and accessible by this API.
     *
     * For each one of those Object reflections, this class provides detailed
     * information about their native origin. For example if an Object
     * represents a native C++ class instante, then it provides access to its
     * C++ class/struct name, to its C++ member variables, its native memory
     * size and much more.
     *
     * Even though this framework allows you to adjust abstract Object instances
     * to a certain extent, most of the methods of this Object class are
     * read-only though and the actual modifyable methods are made available
     * not as part of this Object class, but as part of the Archive class
     * instead. This design decision was made for performance and safety
     * reasons.
     *
     * @see Archive::setIntValue() as an example for modifying Object instances.
     */
    class Object {
    public:
        Object();
        Object(UIDChain uidChain, DataType type, const Object& parent);

        UID uid(int index = 0) const;
        const UIDChain& uidChain() const;
        UID parentUID() const;
        const DataType& type() const;
        const RawData& rawData() const;
        Version version() const;
        Version minVersion() const;
        bool isVersionCompatibleTo(const Object& other) const;
        std::vector<Member>& members();
        const std::vector<Member>& members() const;
        Member memberNamed(String name) const;
        Member memberByUID(const UID& uid) const;
        std::vector<Member> membersOfType(const DataType& type) const;
        int sequenceIndexOf(const Member& member) const;
        bool isValid() const;
        operator bool() const { return isValid(); } ///< Same as calling isValid().
        //bool operator()() const { return isValid(); }
        bool operator==(const Object& other) const;
        bool operator!=(const Object& other) const;
        bool operator<(const Object& other) const;
        bool operator>(const Object& other) const;
        void setNativeValueFromString(const String& s);

    protected:
        void remove(const Member& member);
        void setVersion(Version v);
        void setMinVersion(Version v);

    private:
        DataType m_type;
        UIDChain m_uid;
        UID m_parentUID;
        Version m_version;
        Version m_minVersion;
        RawData m_data;
        std::vector<Member> m_members;
        std::function<void(Object& dstObj, const Object& srcObj, void* syncer)> m_sync;

#if LIBGIG_SERIALIZATION_INTERNAL
        template<typename T>
        friend T _primitiveObjectValueToNumber(const Object& obj);
#endif // LIBGIG_SERIALIZATION_INTERNAL

        friend class Archive;
        friend class SrxFormat;
        friend class SrxJSONDecoder;
    };

    /** @brief Destination container for serialization, and source container for deserialization.
     *
     * This is the main class for implementing serialization and deserialization
     * with your C++ application. This framework does not require a tree
     * structured layout of your C++ objects being serialized/deserialized, it
     * therefore uses a concept of a "root" object. So to start serialization
     * construct an empty Archive object and then instruct it to serialize your
     * C++ objects by pointing it to your "root" object:
     * @code
     * Archive a;
     * a.serialize(&myRootObject);
     * @endcode
     * Or if you prefer the look of operator based code:
     * @code
     * Archive a;
     * a << myRootObject;
     * @endcode
     * The Archive object will then serialize all members of the passed C++
     * object, and will recursively serialize all other C++ objects which it
     * contains or points to. So the root object is the starting point for the
     * overall serialization. After the serialize() method returned, you can
     * then access the serialized data stream by calling rawData() and send that
     * data stream over "wire", or store it on disk or whatever you may intend
     * to do with it.
     *
     * Then on receiver side likewise, you create a new Archive object, pass the
     * received data stream i.e. via constructor to the Archive object and call
     * deserialize() by pointing it to the root object on receiver side:
     * @code
     * Archive a(rawDataStream);
     * a.deserialize(&myRootObject);
     * @endcode
     * Or with operator instead:
     * @code
     * Archive a(rawDataStream);
     * a >> myRootObject;
     * @endcode
     * Now this framework automatically handles serialization and
     * deserialization of fundamental data types (like i.e. @c char, @c int,
     * @c long @c int, @c float, @c double, etc.) and common C++ classes
     * (currently: @c std::string, @c std::vector, @c std::set and @c std::map)
     * automatically for you. However for your own custom C++ classes and
     * structs you must implement one method which defines which members of your
     * class / struct should actually be serialized and deserialized. That
     * method to be added must have the following signature:
     * @code
     * void serialize(Serialization::Archive* archive);
     * @endcode
     * So let's say you have the following simple data structures:
     * @code
     * struct Foo {
     *     int a;
     *     bool b;
     *     double c;
     *     std::string text;
     *     std::vector<int> v;
     * };
     *
     * struct Bar {
     *     char  one;
     *     float two;
     *     Foo   foo1;
     *     Foo*  pFoo2;
     *     Foo*  pFoo3DontTouchMe; // shall not be serialized/deserialized
     *     std::set<int> someSet;
     *     std::map<double,Foo> someMap;
     * };
     * @endcode
     * So in order to be able to serialize and deserialize objects of those two
     * structures you would first add the mentioned method to each struct
     * definition (i.e. in your header file):
     * @code
     * struct Foo {
     *     int a;
     *     bool b;
     *     double c;
     *     std::string text;
     *     std::vector<int> v;
     *
     *     void serialize(Serialization::Archive* archive);
     * };
     *
     * struct Bar {
     *     char  one;
     *     float two;
     *     Foo   foo1;
     *     Foo*  pFoo2;
     *     Foo*  pFoo3DontTouchMe; // shall not be serialized/deserialized
     *     std::set<int> someSet;
     *     std::map<double,Foo> someMap;
     *
     *     void serialize(Serialization::Archive* archive);
     * };
     * @endcode
     * And then you would implement those two new methods like this (i.e. in
     * your .cpp file):
     * @code
     * #define SRLZ(member) \
     *   archive->serializeMember(*this, member, #member);
     *
     * void Foo::serialize(Serialization::Archive* archive) {
     *     SRLZ(a);
     *     SRLZ(b);
     *     SRLZ(c);
     *     SRLZ(text);
     *     SRLZ(v);
     * }
     *
     * void Bar::serialize(Serialization::Archive* archive) {
     *     SRLZ(one);
     *     SRLZ(two);
     *     SRLZ(foo1);
     *     SRLZ(pFoo2);
     *     // leaving out pFoo3DontTouchMe here
     *     SRLZ(someSet);
     *     SRLZ(someMap);
     * }
     * @endcode
     * And that's it!
     *
     * Now when you serialize such a @c Bar object, this framework will also
     * automatically serialize the respective @c Foo object(s) accordingly, also
     * for the @c pFoo2 pointer for instance (as long as it is not a @c NULL
     * pointer that is).
     *
     * Note that there is only one method that you need to implement. So the
     * respective @c serialize() method implementation of your classes/structs
     * are both called for serialization, as well as for deserialization!
     * Usually you don't need to know whether your @c serialize() method was
     * called for serialization or deserialization, however if you need to do
     * know for some reason you can call @c archive->operation() inside your
     * @c serialize() method to distinguish between the two.
     *
     * In case you need to explicitly define and enforce backward
     * incompatibility for one of your C++ classes, you can do so by setting
     * a version and minimum version for your class (see @c setVersion() and
     * @c setMinVersion() for details).
     */
    class Archive {
    public:
        /** @brief Current activity of @c Archive object.
         */
        enum operation_t {
            OPERATION_NONE,        ///< Archive is currently neither serializing, nor deserializing.
            OPERATION_SERIALIZE,   ///< Archive is currently serializing.
            OPERATION_DESERIALIZE  ///< Archive is currently deserializing.
        };

        /** @brief Encoding format of serialized data stream.
         */
        enum format_t {
            /**
             * Automatically handle the encoding format:
             *
             * - When deserializing: auto detect encoding format of raw input
             *   data stream.
             *
             * - When serializing a new Archive: use default encoding format
             *   (currently defaults to: "Srx").
             *
             * - When serializing a previously deserialized Archive: use the
             *   same encoding format that has been used before.
             */
            FORMAT_AUTO = 0,
            /**
             * Use and expect "Srx" encoding format (simple, reliable, fast).
             */
            FORMAT_SRX = 1,
            /**
             * Use and expect "Srx-JSON" encoding format (user-friendly to
             * read and modify).
             */
            FORMAT_SRX_JSON = (1<<1),
        };

        Archive(format_t format = FORMAT_AUTO);
        Archive(const RawData& data, format_t format = FORMAT_AUTO);
        Archive(const uint8_t* data, size_t size, format_t format = FORMAT_AUTO);
        virtual ~Archive();

        /** @brief Initiate serialization.
         *
         * Initiates serialization of all native C++ objects, which means
         * capturing and storing the current data of all your C++ objects as
         * content of this Archive.
         *
         * This framework has a concept of a "root" object which you must pass
         * to this method. The root object is the starting point for
         * serialization of your C++ objects. The framework will then
         * recursively serialize all members of that C++ object an continue to
         * serialize all other C++ objects that it might contain or point to.
         *
         * After this method returned, you might traverse all serialized objects
         * by walking them starting from the rootObject(). You might then modify
         * that abstract reflection of your C++ objects and finally you might
         * call rawData() to get an encoded raw data stream which you might use
         * for sending it "over wire" to somewhere where it is going to be
         * deserialized later on.
         *
         * Note that whenever you call this method, the previous content of this
         * Archive will first be cleared.
         *
         * @param obj - native C++ root object where serialization shall start
         * @see Archive::operator<<()
         */
        template<typename T>
        void serialize(const T* obj) {
            m_operation = OPERATION_SERIALIZE;
            m_allObjects.clear();
            m_rawData.clear();
            preregisterNativeObject(*obj);
            m_root = UID::from(obj);
            const_cast<T*>(obj)->serialize(this);
            encode();
            m_operation = OPERATION_NONE;
        }

        /** @brief Initiate deserialization.
         *
         * Initiates deserialization of all native C++ objects, which means all
         * your C++ objects will be restored with the values contained in this
         * Archive. So that also means calling deserialize() only makes sense if
         * this a non-empty Archive, which i.e. is the case if you either called
         * serialize() with this Archive object before or if you passed a
         * previously serialized raw data stream to the constructor of this
         * Archive object.
         * 
         * This framework has a concept of a "root" object which you must pass
         * to this method. The root object is the starting point for
         * deserialization of your C++ objects. The framework will then
         * recursively deserialize all members of that C++ object an continue to
         * deserialize all other C++ objects that it might contain or point to,
         * according to the values stored in this Archive.
         *
         * @param obj - native C++ root object where deserialization shall start
         * @see Archive::operator>>()
         *
         * @throws Exception if the data stored in this Archive cannot be
         *         restored to the C++ objects passed to this method, i.e.
         *         because of version or type incompatibilities.
         */
        template<typename T>
        void deserialize(T* obj) {
            Archive a;
            a.m_operation = m_operation = OPERATION_DESERIALIZE;
            a.preregisterNativeObject(*obj);
            obj->serialize(&a);
            a.m_root = UID::from(obj);
            Syncer s(a, *this);
            a.m_operation = m_operation = OPERATION_NONE;
        }

        /** @brief Initiate serialization of your C++ objects.
         *
         * Same as calling @c serialize(), this is just meant if you prefer
         * to use operator based code instead, which you might find to be more
         * intuitive.
         *
         * Example:
         * @code
         * Archive a;
         * a << myRootObject;
         * @endcode
         *
         * @see Archive::serialize() for more details.
         */
        template<typename T>
        void operator<<(const T& obj) {
            serialize(&obj);
        }

        /** @brief Initiate deserialization of your C++ objects.
         *
         * Same as calling @c deserialize(), this is just meant if you prefer
         * to use operator based code instead, which you might find to be more
         * intuitive.
         *
         * Example:
         * @code
         * Archive a(rawDataStream);
         * a >> myRootObject;
         * @endcode
         *
         * @throws Exception if the data stored in this Archive cannot be
         *         restored to the C++ objects passed to this method, i.e.
         *         because of version or type incompatibilities.
         *
         * @see Archive::deserialize() for more details.
         */
        template<typename T>
        void operator>>(T& obj) {
            deserialize(&obj);
        }

        const RawData& rawData();
        virtual String rawDataFormat() const;

        /** @brief Serialize a native C/C++ member variable.
         *
         * This method is usually called by the serialize() method
         * implementation of your C/C++ structs and classes, for each of the
         * member variables that shall be serialized and deserialized
         * automatically with this framework. It is recommend that you are not
         * using this method name directly, but rather define a short hand C
         * macro in your .cpp file like:
         * @code
         * #define SRLZ(member) \
         *   archive->serializeMember(*this, member, #member);
         *
         * struct Foo {
         *     int a;
         *     Bar b; // a custom struct or class having a serialize() method
         *     std::string c;
         *     std::vector<double> d;
         *
         *     void serialize(Serialization::Archive* archive);
         * };
         *
         * void Foo::serialize(Serialization::Archive* archive) {
         *     SRLZ(a);
         *     SRLZ(b);
         *     SRLZ(c);
         *     SRLZ(d);
         * }
         * @endcode
         * As you can see, using such a macro makes your code more readable,
         * compact and less error prone.
         *
         * It is completely up to you to decide which ones of your member
         * variables shall automatically be serialized and deserialized with
         * this framework. Only those member variables which are registered by
         * calling this method will be serialized and deserialized. It does not
         * really matter in which order you register your individiual member
         * variables by calling this method, but the sequence is actually stored
         * as meta information with the resulting archive and the resulting raw
         * data stream. That meta information might then be used by this
         * framework to automatically correct and adapt deserializing that
         * archive later on for a future (or older) and potentially heavily
         * modified version of your software. So it is recommended, even though
         * also not required, that you may retain the sequence of your
         * serializeMember() calls for your individual C++ classes' members over
         * all your software versions, to retain backward compatibility of older
         * archives as much as possible.
         *
         * @param nativeObject - native C++ object to be registered for
         *                       serialization / deserialization
         * @param nativeMember - native C++ member variable of @a nativeObject
         *                       to be registered for serialization /
         *                       deserialization
         * @param memberName - name of @a nativeMember to be stored with this
         *                     archive
         * @see serializeHeapMember() for variables on the RAM heap
         */
        template<typename T_classType, typename T_memberType>
        void serializeMember(const T_classType& nativeObject, const T_memberType& nativeMember, const char* memberName) {
            Object& parent = preregisterNativeObject(nativeObject);
            const ssize_t offset =
                ((const uint8_t*)(const void*)&nativeMember) -
                ((const uint8_t*)(const void*)&nativeObject);
            const UIDChain uids = UIDChainResolver<T_memberType>(nativeMember);
            const DataType type = DataType::dataTypeOf(nativeMember);
            const Member member(memberName, uids[0], offset, type, parent);
            std::vector<Member>& members = parent.members();
            for (const Member& m : members)
                assert(m.name() != memberName);
            parent.members().push_back(member);
            const Object obj(uids, type, parent);
            const bool bExistsAlready = m_allObjects.count(uids[0]);
            const bool isValidObject = obj;
            const Object& existingObj = m_allObjects[uids[0]];
            const bool bExistingObjectIsInvalid = !existingObj;
            if (isValidObject && (
                    !bExistsAlready || bExistingObjectIsInvalid || (
                        obj.parentUID() && !existingObj.parentUID()
                    )
               ))
            {
                m_allObjects[uids[0]] = obj;
                // recurse serialization for all members of this member
                // (only for struct/class types, noop for primitive types)
                SerializationRecursion<T_memberType>::serializeObject(this, nativeMember);
            }
        }

        /** @brief Serialize a C/C++ member variable allocated on the heap.
         *
         * This method is essentially used by applications in the same way as
         * @c serializeMember() above, however @c serializeMember() must only be
         * used for native C/C++ members which are variables that are memory
         * located within their owning parent data structures. For any variable
         * that's located on the RAM heap though, applications must use this
         * method instead to make it clear that there's no constant, static
         * offset relationship between the passed member and its owning parent.
         *
         * @discussion To avoid member name conflicts with native members, it is
         * recommended to always choose member names which would be impossible
         * as names to be declared in C/C++ code. For instance this framework
         * uses heap member names "[0]", "[1]", "[2]", ... in its out of the box
         * support when serializing elements of @c Array<> objects, since
         * brackets in general cannot be used as part of variable names in C++,
         * so using such or other special characters in heap member names, makes
         * such naming conflicts impossible.
         *
         * @param nativeObject - native C++ object to be registered for
         *                       serialization / deserialization
         * @param heapMember - C/C++ variable (located on the heap) of
         *                     @a nativeObject to be registered for
         *                     serialization / deserialization
         * @param memberName - name of @a heapMember to be stored with this
         *                     archive; an arbitrary but unique name should be
         *                     chosen which must not collide with names of
         *                     native members (see discussion above)
         * @see serializeMember() for native member variables
         */
        template<typename T_classType, typename T_memberType>
        void serializeHeapMember(const T_classType& nativeObject, const T_memberType& heapMember, const char* memberName) {
            Object& parent = preregisterNativeObject(nativeObject);
            const ssize_t offset = -1; // used for all members on heap
            const UIDChain uids = UIDChainResolver<T_memberType>(heapMember);
            const DataType type = DataType::dataTypeOf(heapMember);
            const Member member(memberName, uids[0], offset, type, parent);
            parent.members().push_back(member);
            const Object obj(uids, type, parent);
            const bool bExistsAlready = m_allObjects.count(uids[0]);
            const bool isValidObject = obj;
            const Object& existingObj = m_allObjects[uids[0]];
            const bool bExistingObjectIsInvalid = !existingObj;
            if (isValidObject && (
                    !bExistsAlready || bExistingObjectIsInvalid || (
                        obj.parentUID() && !existingObj.parentUID()
                    )
               ))
            {
                m_allObjects[uids[0]] = obj;
                // recurse serialization for all members of this member
                // (only for struct/class types, noop for primitive types)
                SerializationRecursion<T_memberType>::serializeObject(this, heapMember);
            }
        }

        /** @brief Serialize anonymous C/C++ data.
         *
         * Serializes data that's an arbitrary memory region being pointed at,
         * i.e. by a previously serialized native pointer variable.
         *
         * @b NOTE: Usually this method is not called directly by applications.
         * This method is called automatically by this framework when
         * serializing a (non-NULL) pointer variable.
         *
         * This method can be called both for anonymous data on the heap, as
         * well as for a foreign object's member variable being pointed at.
         *
         * This framework automatically identifies strong vs. weak pointers to
         * memory regions by the general data layout being analyzed, which is
         * important when deserializing a data stream into a native application:
         * For identified strong pointers the respective native data is
         * allocated and initialized automatically on deserialization. Weak
         * pointers on the other hand just point at data contained within
         * another, already deserialized object.
         */
        template<typename T_objectType>
        void serializeAnonymousObject(const T_objectType& heapObject) {
            const UIDChain uids = UIDChainResolver<T_objectType>(heapObject);
            const DataType type = DataType::dataTypeOf(heapObject);
            const Object obj(uids, type, Object());
            const bool bExistsAlready = m_allObjects.count(uids[0]);
            const bool isValidObject = obj;
            const Object& existingObj = m_allObjects[uids[0]];
            const bool bExistingObjectIsInvalid = !existingObj;
            if (isValidObject && (
                    !bExistsAlready || bExistingObjectIsInvalid || (
                        obj.parentUID() && !existingObj.parentUID()
                    )
               ))
            {
                m_allObjects[uids[0]] = obj;
                // recurse serialization for all members of this object
                // (only for struct/class types, noop for primitive types)
                SerializationRecursion<T_objectType>::serializeObject(this, heapObject);
            }
        }

        /** @brief Set current version number for your C++ class.
         *
         * By calling this method you can define a version number for your
         * current C++ class (that is a version for its current data structure
         * layout and method implementations) that is going to be stored along
         * with the serialized archive. Only call this method if you really want
         * to constrain compatibility of your C++ class.
         *
         * Along with calling @c setMinVersion() this provides a way for you
         * to constrain backward compatibility regarding serialization and
         * deserialization of your C++ class which the Archive class will obey
         * to. If required, then typically you might do so in your
         * @c serialize() method implementation like:
         * @code
         * #define SRLZ(member) \
         *   archive->serializeMember(*this, member, #member);
         *
         * struct Foo {
         *     int a;
         *     Bar b; // a custom struct or class having a serialize() method
         *     std::string c;
         *     std::vector<double> d;
         *
         *     void serialize(Serialization::Archive* archive);
         * };
         *
         * void Foo::serialize(Serialization::Archive* archive) {
         *     // when serializing: the current version of this class that is
         *     // going to be stored with the serialized archive
         *     archive->setVersion(*this, 6);
         *     // when deserializing: the minimum version this C++ class is
         *     // compatible with
         *     archive->setMinVersion(*this, 3);
         *     // actual data mebers to serialize / deserialize
         *     SRLZ(a);
         *     SRLZ(b);
         *     SRLZ(c);
         *     SRLZ(d);
         * }
         * @endcode
         * In this example above, the C++ class "Foo" would be serialized along
         * with the version number @c 6 and minimum version @c 3 as additional
         * meta information in the resulting archive (and its raw data stream
         * respectively).
         *
         * When deserializing archives with the example C++ class code above,
         * the Archive object would check whether your originally serialized
         * C++ "Foo" object had at least version number @c 3, if not the
         * deserialization process would automatically be stopped with a
         * @c Serialization::Exception, claiming that the classes are version
         * incompatible.
         *
         * But also consider the other way around: you might have serialized
         * your latest version of your C++ class, and might deserialize that
         * archive with an older version of your C++ class. In that case it will
         * likewise be checked whether the version of that old C++ class is at
         * least as high as the minimum version set with the already seralized
         * bleeding edge C++ class.
         *
         * Since this Serialization / deserialization framework is designed to
         * be robust on changes to your C++ classes and aims trying to
         * deserialize all your C++ objects correctly even if your C++ classes
         * have seen substantial software changes in the meantime; you might
         * sometimes see it as necessary to constrain backward compatibility
         * this way. Because obviously there are certain things this framework
         * can cope with, like for example that you renamed a data member while
         * keeping the layout consistent, or that you have added new members to
         * your C++ class or simply changed the order of your members in your
         * C++ class. But what this framework cannot detect is for example if
         * you changed the semantics of the values stored with your members, or
         * even substantially changed the algorithms in your class methods such
         * that they would not handle the data of your C++ members in the same
         * and correct way anymore.
         *
         * @param nativeObject - your C++ object you want to set a version for
         * @param v - the version number to set for your C++ class (by default,
         *            that is if you do not explicitly call this method, then
         *            your C++ object will be stored with version number @c 0 ).
         */
        template<typename T_classType>
        void setVersion(const T_classType& nativeObject, Version v) {
            const UID uid = UID::from(nativeObject);
            Object& obj = m_allObjects[uid];
            if (!obj) {
                const UIDChain uids = UIDChainResolver<T_classType>(nativeObject);
                const DataType type = DataType::dataTypeOf(nativeObject);
                obj = Object(uids, type, Object());
            }
            setVersion(obj, v);
        }

        /** @brief Set a minimum version number for your C++ class.
         *
         * Call this method to define a minimum version that your current C++
         * class implementation would be compatible with when it comes to 
         * deserialization of an archive containing an object of your C++ class.
         * Like the version information, the minimum version will also be stored
         * for objects of your C++ class with the resulting archive (and its
         * resulting raw data stream respectively).
         *
         * When you start to constrain version compatibility of your C++ class
         * you usually start by using 1 as version and 1 as minimum version.
         * So it is eligible to set the same number to both version and minimum
         * version. However you must @b not set a minimum version higher than
         * version. Doing so would not raise an exception, but the resulting
         * behavior would be undefined.
         *
         * It is not relevant whether you first set version and then minimum
         * version or vice versa. It is also not relevant when exactly you set
         * those two numbers, even though usually you would set both in your
         * serialize() method implementation.
         *
         * @see @c setVersion() for more details about this overall topic.
         *
         * @param nativeObject - your C++ object you want to set a version for
         * @param v - the minimum version you want to define for your C++ class
         *            (by default, that is if you do not explicitly call this
         *            method, then a minium version of @c 0 is assumed for your
         *            C++ class instead).
         */
        template<typename T_classType>
        void setMinVersion(const T_classType& nativeObject, Version v) {
            const UID uid = UID::from(nativeObject);
            Object& obj = m_allObjects[uid];
            if (!obj) {
                const UIDChain uids = UIDChainResolver<T_classType>(nativeObject);
                const DataType type = DataType::dataTypeOf(nativeObject);
                obj = Object(uids, type, Object());
            }
            setMinVersion(obj, v);
        }

        virtual void decode(const RawData& data);
        virtual void decode(const RawData& data, format_t format);
        virtual void decode(const uint8_t* data, size_t size);
        virtual void decode(const uint8_t* data, size_t size, format_t format);
        void clear();
        bool isModified() const;
        void removeMember(Object& parent, const Member& member);
        void remove(const Object& obj);
        Object& rootObject();
        Object& objectByUID(const UID& uid);
        Object& parentObjectOf(const Object& obj);
        Object& parentObjectOf(const Member& member);
        void setAutoValue(Object& object, String value);
        void setIntValue(Object& object, int64_t value);
        void setRealValue(Object& object, double value);
        void setCharValue(Object& object, char value);
        void setBoolValue(Object& object, bool value);
        void setEnumValue(Object& object, uint64_t value);
        void setStringValue(Object& object, String value);
        String valueAsString(const Object& object);
        int64_t valueAsInt(const Object& object);
        double valueAsReal(const Object& object);
        char valueAsChar(const Object& object);
        bool valueAsBool(const Object& object);
        void setVersion(Object& object, Version v);
        void setMinVersion(Object& object, Version v);
        String name() const;
        void setName(String name);
        String comment() const;
        void setComment(String comment);
        time_t timeStampCreated() const;
        time_t timeStampModified() const;
        tm dateTimeCreated(time_base_t base = LOCAL_TIME) const;
        tm dateTimeModified(time_base_t base = LOCAL_TIME) const;
        operation_t operation() const;

    protected:
        Object& objectByBaseUID(const UID& uid);
        static String primitiveObjectValueToString(const Object& obj);

        template<typename T_classType>
        Object& preregisterNativeObject(const T_classType& nativeObject) {
            const UID uid = UID::from(nativeObject);
            Object& obj = m_allObjects[uid];
            if (!obj) {
                const UIDChain uids = UIDChainResolver<T_classType>(nativeObject);
                const DataType type = DataType::dataTypeOf(nativeObject);
                obj = Object(uids, type, Object());
            }
            return obj;
        }

        // UID resolver for non-pointer types
        template<typename T>
        class UIDChainResolver {
        public:
            UIDChainResolver(const T& data) {
                m_uid.push_back(UID::from(data));
            }

            operator UIDChain() const { return m_uid; }
            UIDChain operator()() const { return m_uid; }
        private:
            UIDChain m_uid;
        };

        // UID resolver for pointer types (of 1st degree)
        template<typename T>
        class UIDChainResolver<T*> {
        public:
            UIDChainResolver(const T* const & data) {
                m_uid.push_back({ (ID) &data, sizeof(data) });
                if (data)
                    m_uid.push_back(UID::from(*data));
                else
                    m_uid.push_back({ (ID) data, sizeof(*data) });
            }

            operator UIDChain() const { return m_uid; }
            UIDChain operator()() const { return m_uid; }
        private:
            UIDChain m_uid;
        };

        // SerializationRecursion for non-pointer class/struct types.
        template<typename T, bool T_isRecursive>
        struct SerializationRecursionImpl {
            static void serializeObject(Archive* archive, const T& obj) {
                const_cast<T&>(obj).serialize(archive);
            }
        };

        // SerializationRecursion for pointers (of 1st degree) to class/structs.
        template<typename T, bool T_isRecursive>
        struct SerializationRecursionImpl<T*,T_isRecursive> {
            static void serializeObject(Archive* archive, const T*& obj) {
                if (!obj) return;
                const_cast<T*&>(obj)->serialize(archive);
            }
        };

        // NOOP SerializationRecursion for primitive types.
        template<typename T>
        struct SerializationRecursionImpl<T,false> {
            static void serializeObject(Archive* /*archive*/, const T& /*obj*/) {}
        };

        // SerializationRecursion for pointers (of 1st degree) to primitive types.
        template<typename T>
        struct SerializationRecursionImpl<T*,false> {
            static void serializeObject(Archive* archive, const T* const & obj) {
                if (!obj) return;
                archive->serializeAnonymousObject(*obj);
            }
        };

        // NOOP SerializationRecursion for String objects.
        template<bool T_isRecursive>
        struct SerializationRecursionImpl<String,T_isRecursive> {
            static void serializeObject(Archive* archive, const String& obj) {}
        };

        // SerializationRecursion for String pointers (of 1st degree).
        template<bool T_isRecursive>
        struct SerializationRecursionImpl<String*,T_isRecursive> {
            static void serializeObject(Archive* archive, const String*& obj) {
                if (!obj) return;
                archive->serializeAnonymousObject(*obj);
            }
        };

        // SerializationRecursion for Array<> objects.
        template<typename T, bool T_isRecursive>
        struct SerializationRecursionImpl<Array<T>,T_isRecursive> {
            static void serializeObject(Archive* archive, const Array<T>& obj) {
                const UIDChain uids = UIDChainResolver<Array<T>>(obj);
                const Object& object = archive->objectByUID(uids[0]);
                if (archive->operation() == OPERATION_SERIALIZE) {
                    for (size_t i = 0; i < obj.size(); ++i) {
                        archive->serializeHeapMember(
                            obj, obj[i], ("[" + toString(i) + "]").c_str()
                        );
                    }
                } else {
                    const_cast<Object&>(object).m_sync =
                        [&obj,archive](Object& dstObj, const Object& srcObj,
                                       void* syncer)
                    {
                        const size_t n = srcObj.members().size();
                        const_cast<Array<T>&>(obj).resize(n);
                        for (size_t i = 0; i < n; ++i) {
                            archive->serializeHeapMember(
                                obj, obj[i], ("[" + toString(i) + "]").c_str()
                            );
                        }
                        // updating dstObj required as serializeHeapMember()
                        // replaced the original object by a new one
                        dstObj = archive->objectByUID(dstObj.uid());
                        for (size_t i = 0; i < n; ++i) {
                            String name = "[" + toString(i) + "]";
                            Member srcMember = srcObj.memberNamed(name);
                            Member dstMember = dstObj.memberNamed(name);
                            ((Syncer*)syncer)->syncMember(dstMember, srcMember);
                        }
                    };
                }
            }
        };

        // SerializationRecursion for Array<> pointers (of 1st degree).
        template<typename T, bool T_isRecursive>
        struct SerializationRecursionImpl<Array<T>*,T_isRecursive> {
            static void serializeObject(Archive* archive, const Array<T>*& obj) {
                if (!obj) return;
                SerializationRecursionImpl<Array<T>,T_isRecursive>::serializeObject(
                    archive, *obj
                );
            }
        };

        // SerializationRecursion for Set<> objects.
        template<typename T, bool T_isRecursive>
        struct SerializationRecursionImpl<Set<T>,T_isRecursive> {
            static void serializeObject(Archive* archive, const Set<T>& obj) {
                const UIDChain uids = UIDChainResolver<Set<T>>(obj);
                const Object& object = archive->objectByUID(uids[0]);
                if (archive->operation() == OPERATION_SERIALIZE) {
                    for (const T& key : obj) {
                        archive->serializeHeapMember(
                            obj, key, ("[" + toString(key) + "]").c_str()
                        );
                    }
                } else {
                    const_cast<Object&>(object).m_sync =
                        [&obj,archive](Object& dstObj, const Object& srcObj,
                                       void* syncer)
                    {
                        const size_t n = srcObj.members().size();
                        const_cast<Set<T>&>(obj).clear();
                        for (size_t i = 0; i < n; ++i) {
                            const Member& member = srcObj.members()[i];
                            String name = member.name();
                            // strip brackets from name
                            if (name.length() < 2 || name[0] != '[' ||
                                *name.rbegin() != ']') continue;
                            name = name.substr(1, name.length() - 2);
                            T key = T();
                            const UIDChain uids = UIDChainResolver<T>(key);
                            const DataType type = DataType::dataTypeOf(key);
                            Object tmpObj(uids, type, Object());
                            // set the value of "key" variable by abstraction API
                            tmpObj.setNativeValueFromString(name);
                            // only for keys of pointer type: translation of
                            // memory address from source archive to destination
                            // archive required, NOOP for all other data types
                            ((Syncer*)syncer)->translateKey(key);
                            // insert (translated) key into set
                            const_cast<Set<T>&>(obj).insert(key);
                        }
                        // continue serialization recursion
                        for (const T& key : obj) {
                            archive->serializeHeapMember(
                                obj, key, ("[" + toString(key) + "]").c_str()
                            );
                        }
                        // updating dstObj required as serializeHeapMember()
                        // replaced the original object by a new one
                        dstObj = archive->objectByUID(dstObj.uid());
                    };
                }
            }
        };

        // SerializationRecursion for Set<> pointers (of 1st degree).
        template<typename T, bool T_isRecursive>
        struct SerializationRecursionImpl<Set<T>*,T_isRecursive> {
            static void serializeObject(Archive* archive, const Set<T>*& obj) {
                if (!obj) return;
                SerializationRecursionImpl<Set<T>,T_isRecursive>::serializeObject(
                    archive, *obj
                );
            }
        };

        // SerializationRecursion for Map<> objects.
        template<typename T_key, typename T_value, bool T_isRecursive>
        struct SerializationRecursionImpl<Map<T_key,T_value>,T_isRecursive> {
            static void serializeObject(Archive* archive, const Map<T_key,T_value>& obj) {
                const UIDChain uids = UIDChainResolver<Map<T_key,T_value>>(obj);
                const Object& object = archive->objectByUID(uids[0]);
                if (archive->operation() == OPERATION_SERIALIZE) {
                    for (const auto& it : obj) {
                        // recurse serialization for map's key ...
                        SerializationRecursion<T_key>::serializeObject(archive, it.first);
                        // ... and map's value
                        archive->serializeHeapMember(
                            obj, it.second, ("[" + toString(it.first) + "]").c_str()
                        );
                    }
                } else {
                    const_cast<Object&>(object).m_sync =
                        [&obj,archive](Object& dstObj, const Object& srcObj,
                                       void* syncer)
                    {
                        const size_t n = srcObj.members().size();
                        const_cast<Map<T_key,T_value>&>(obj).clear();
                        // translation is neutral except for keys of pointer type (see comments below)
                        std::map<String,String> memberNameTranslation;
                        for (size_t i = 0; i < n; ++i) {
                            const Member& member = srcObj.members()[i];
                            String name = member.name();
                            // strip brackets from name
                            if (name.length() < 2 || name[0] != '[' ||
                                *name.rbegin() != ']') continue;
                            name = name.substr(1, name.length() - 2);
                            T_key srcKey = T_key();
                            const UIDChain uids = UIDChainResolver<T_key>(srcKey);
                            const DataType type = DataType::dataTypeOf(srcKey);
                            Object tmpObj(uids, type, Object());
                            // set the value of "srcKey" variable by abstraction API
                            tmpObj.setNativeValueFromString(name);
                            T_key dstKey = srcKey;
                            // only for keys of pointer type: translation of
                            // memory address from source archive to destination
                            // archive required, NOOP for all other data types
                            ((Syncer*)syncer)->translateKey(dstKey);
                            memberNameTranslation[member.name()] = "[" + toString(dstKey) + "]";
                            // insert (translated) key into the destination map
                            const_cast<Map<T_key,T_value>&>(obj)[dstKey] = T_value();
                        }
                        // continue serialization recursion ...
                        for (const auto& it : obj) {
                            // ... for map's key ...
                            SerializationRecursion<T_key>::serializeObject(archive, it.first);
                            // ... and map's value
                            archive->serializeHeapMember(
                                obj, it.second, ("[" + toString(it.first) + "]").c_str()
                            );
                        }
                        // updating dstObj required as serializeHeapMember()
                        // replaced the original object by a new one
                        dstObj = archive->objectByUID(dstObj.uid());
                        // sync map's values
                        for (size_t i = 0; i < n; ++i) {
                            Member srcMember = srcObj.members()[i];
                            Member dstMember = dstObj.memberNamed(
                                memberNameTranslation[srcMember.name()]
                            );
                            ((Syncer*)syncer)->syncMember(dstMember, srcMember);
                        }
                    };
                }
            }
        };

        // SerializationRecursion for Map<> pointers (of 1st degree).
        template<typename T_key, typename T_value, bool T_isRecursive>
        struct SerializationRecursionImpl<Map<T_key,T_value>*,T_isRecursive> {
            static void serializeObject(Archive* archive, const Map<T_key,T_value>*& obj) {
                if (!obj) return;
                SerializationRecursionImpl<Map<T_key,T_value>,T_isRecursive>::serializeObject(
                    archive, *obj
                );
            }
        };

        // Automatically handles recursion for class/struct types, while ignoring all primitive types.
        template<typename T>
        struct SerializationRecursion : SerializationRecursionImpl<T, LIBGIG_IS_CLASS(T)> {
        };

        class ObjectPool : public std::map<UID,Object> {
        public:
            // prevent passing obvious invalid UID values from creating a new pair entry
            Object& operator[](const UID& k) {
                if (!k.isValid())
                    return invalidObject();
                return std::map<UID,Object>::operator[](k);
            }

            static Object& invalidObject() {
                static Object invalid;
                invalid = Object();
                return invalid;
            }
        };

    protected:
        /** @brief Synchronizes 2 archives with each other.
         *
         * This class is used internally at final stage of deserialization. It
         * is not used for serialization.
         *
         * The deserialization algorithm of this framework works like this:
         *
         * 1. The (currently running) application constructs an @c Archive
         *    object by passing a previously serialized raw data stream to the
         *    @c Archive constructor. At this stage, the raw data stream is
         *    decoded and this archive's object pool is populated with objects,
         *    which in turn are filled with data (at temporary storage
         *    location inside the respective @c Object instances) of the decoded
         *    data stream.
         *
         * 2. A temporary (2nd) @c Archive object is constructed internally by
         *    this framework, reflecting the (currently running) application's
         *    latest data structre layout, without touching any actual data yet.
         *    The individual @c Object instances of this 2nd @c Archive are
         *    bound to the running application's native (target) C/C++ member
         *    variables to be updated (written to) next.
         *
         * Note that at this point, the 2 archives might very well have quite
         * different data structure layouts, due to potential software changes
         * between the original serializing application and this currently
         * deserializing software application.
         *
         * 3. This Syncer class is used to transfer the actual data from the 1st
         *    to the 2nd (temporary) @c Archive object, it does so by traversing
         *    the 2 archives' object pools, trying to find the respective 2
         *    objects on the two sides to be synchronized, and if found, it
         *    transfers the data from the 1st archive's object to the 2nd
         *    archive's object, effectively writing to the currently running
         *    application's final C/C++ memory locations.
         *
         * This 3 staged approach allows to deserialize data in a much more
         * relaxed, adaptive and flexible (while still automatic) way than
         * traditional serialization frameworks would do.
         */
        class Syncer {
        public:
            Syncer(Archive& dst, Archive& src);
            void syncObject(const Object& dst, const Object& src);
            void syncPrimitive(const Object& dst, const Object& src);
            void syncString(const Object& dst, const Object& src);
            void syncArray(const Object& dst, const Object& src);
            void syncSet(const Object& dst, const Object& src);
            void syncMap(const Object& dst, const Object& src);
            void syncPointer(const Object& dst, const Object& src);
            void syncMember(const Member& dstMember, const Member& srcMember);

            // Address translation for keys of pointer type.
            //
            // Translates memory address from source archive, to memory address
            // of destination archive. For "strong" pointers memory is allocated
            // and the pointed data is synced with data from source archive.
            template<typename T>
            void translateKey(T*& key) {
                if (!key) return; // NULL pointer on source side

                // UIDChainResolver can't be used here, since the passed memory
                // address is an abstract one that originates from the source
                // archive (e.g. potentially even from another machine) and
                // UIDChainResolver would then crash on its typeid() operator.
                UID uid = { (ID) key, sizeof(*key) };

                const Object& pointedSrcObject = m_src.objectByBaseUID(uid);
                assert(pointedSrcObject);
                std::map<UID,UID>::iterator uidRelation = m_counterparts.find(uid);
                if (pointedSrcObject.parentUID() || uidRelation != m_counterparts.end()) { // "weak" pointer to object ...
                    assert(uidRelation != m_counterparts.end());
                    key = (T*) uidRelation->second.id;
                } else { // "strong" pointer to object, allocation required ...
                    assert(pointedSrcObject.type());
                    Object pointedDstObject = pointedSrcObject.type().newInstance(&m_dst);
                    assert(pointedDstObject);
                    m_dst.m_allObjects[pointedDstObject.uid()] = pointedDstObject;
                    key = (T*) pointedDstObject.uid().id;

                    syncObject(pointedDstObject, pointedSrcObject);
                }
            }

            // NOOP, no key translation required for any other type
            template<typename T>
            void translateKey(T& key) {}
        protected:
            static Member dstMemberMatching(const Object& dstObj, const Object& srcObj, const Member& srcMember);
        private:
            Archive& m_dst;
            Archive& m_src;
            std::map<UID,UID> m_counterparts;
        };

        virtual void encode();
        virtual void encode(format_t format);

        ObjectPool m_allObjects;
        format_t m_format;
        operation_t m_operation;
        UID m_root;
        RawData m_rawData;
        bool m_isModified;
        String m_name;
        String m_comment;
        time_t m_timeCreated;
        time_t m_timeModified;

        friend class DataType;
        friend class SrxFormat;
        friend class SrxJSONEncoder;
        friend class SrxJSONDecoder;
    };

    // UID resolver implementation for non-pointer types
    template<typename T>
    UID UID::Resolver<T>::resolve(const T& obj) {
        const size_t size = DataType::sizeOf(obj);
        const UID uid = { (ID) &obj, size };
        return uid;
    }

    // UID resolver implementation for pointer types (of 1st degree)
    template<typename T>
    UID UID::Resolver<T*>::resolve(const T* const & obj) {
        const size_t size = DataType::sizeOf(*obj);
        const UID uid = { (ID) obj, size };
        return uid;
    }

    // Manual registration of native data types by application.
    template<typename T>
    void DataType::registerNativeDataType() {
        static_assert(
            std::is_default_constructible<T>::value,
            "missing default constructor for type: registration of data "
            "types is only needed for creating instances of that type at "
            "runtime by this framework's reflection API; which in turn "
            "requires the type to provide a default constructor."
        );
        const DataType staticType = DataType::dataType<T>(false);
        assert(staticType);
        const auto itType = m_nativeTypes.find(staticType.internalID());
        if (itType != m_nativeTypes.end()) {
            assert(itType->second.size >= sizeof(T));
            return;
        }

        // register lambda function for allocating a new instance of this type
        m_nativeTypes[staticType.internalID()] = {
            /*.size =*/ sizeof(T),
            /*.allocFn =*/ [](Archive* archive) -> Object {
                T* instance = new T;
                Archive::SerializationRecursion<T>::serializeObject(archive, *instance);
                const UIDChain uids = Archive::UIDChainResolver<T>(*instance);
                Object& obj = archive->objectByUID(uids[0]);
                if (obj) return obj;
                const DataType type = DataType::dataTypeOf(*instance);
                return Object(uids, type, Object());
            }
        };
    }

    // Automatic self-registration of native data types by this framework.
    // (SFINAE variant for types WITH default constructor)
    template<typename T, typename std::enable_if<
             !std::is_pointer<T>::value &&
             std::is_default_constructible<T>::value, bool>::type>
    void DataType::registerNativeDataType(const DataType& rttiType, const T& nativeData) {
        if (!rttiType) return;
        if (m_nativeTypes.find(rttiType.internalID()) != m_nativeTypes.end())
            return;

        // Ensure compile-time native data type 'T' matches passed runtime
        // native data type 'nativeData', because we can only offer to create
        // new instances of native data types by our reflection API if this
        // framework is aware about the data type already. Usually this happens
        // automatically without applications having to do anything. But
        // this only works for types this framework gets in touch with at
        // compile-time.
        //
        // Root problem of all of this: ATM there is no way in C++ to create new
        // instances of a type by using RTTI (typeid(), std::type_info, etc.).
        // If there is a way in a future C++ version then we can get rid of
        // manual type registration altogether. Bur for now we must use new T,
        // which is a compile-time only construct.
        //
        // See the discussion in DataType::registerNativeDataType() for an
        // example where manual type registration would be required.
        const DataType staticType = DataType::dataType<T>(false);
        if (staticType != rttiType) {
            fprintf(stderr, "Serialization Failure: run-time data type '%s' does not match compile-time data type '%s'!\n",
                    rttiType.asLongDescr().c_str(),
                    staticType.asLongDescr().c_str());
            assert(false && "You may need to explicitly register this run-time "
                            "data type by either calling "
                            "DataType::registerNativeType<T>() or using class "
                            "NativeDataTypeRegistry");
        }

        // register lambda function for allocating a new instance of this type
        m_nativeTypes[rttiType.internalID()] = {
            /*.size =*/ sizeof(T),
            /*.allocFn =*/ [](Archive* archive) -> Object {
                T* instance = new T;
                Archive::SerializationRecursion<T>::serializeObject(archive, *instance);
                const UIDChain uids = Archive::UIDChainResolver<T>(*instance);
                Object& obj = archive->objectByUID(uids[0]);
                if (obj) return obj;
                const DataType type = DataType::dataTypeOf(*instance);
                return Object(uids, type, Object());
            }
        };
    }

    // Automatic self-registration of native data types by this framework.
    // (SFINAE variant for types WITHOUT default constructor)
    template<typename T, typename std::enable_if<
             !std::is_pointer<T>::value &&
             !std::is_default_constructible<T>::value, bool>::type>
    void DataType::registerNativeDataType(const DataType& type, const T& nativeData) {
        if (!type) return;
        if (m_nativeTypes.find(type.internalID()) != m_nativeTypes.end())
            return;
        m_nativeTypes[type.internalID()] = {
            /*.size =*/ sizeof(T),
            /*.allocFn =*/ [](Archive*) -> Object {
                assert(false && "instance not possible: native data type does not have default constructor");
                return Object();
            }
        };
    }

    // Automatic self-registration of native data types by this framework.
    // (SFINAE variant for pointer types of 1st degree WITH default constructor)
    template<typename T, typename std::enable_if<
             std::is_default_constructible<T>::value, bool>::type>
    void DataType::registerNativeDataType(const DataType& type,
                                          const T* const& nativeData)
    {
        // register static type
        T unused = T();
        const DataType staticType = DataType::Resolver<T>::resolve(unused);
        registerNativeDataType(staticType, unused);
        // register rtti type
        if (!nativeData) return;
        const DataType rttiType = DataType::Resolver<T>::resolve(*nativeData);
        registerNativeDataType(rttiType, *nativeData);
    }

    // Automatic self-registration of native data types by this framework.
    // (SFINAE variant for pointer types of 1st degree WITHOUT default constructor)
    template<typename T, typename std::enable_if<
             !std::is_default_constructible<T>::value, bool>::type>
    void DataType::registerNativeDataType(const DataType& type,
                                          const T* const& nativeData)
    {
        // register rtti type only
        if (!nativeData) return;
        const DataType nonPointerType = DataType::Resolver<T>::resolve(*nativeData);
        registerNativeDataType(nonPointerType, *nativeData);
    }

    // DataType resolver implementation for primitive / built-in types
    template<typename T, bool T_isPointer>
    DataType DataType::ResolverBase<T,T_isPointer>::resolve(const T& data) {
        // for pointers we must stick to the compile-time declared type
        // of pointer to prevent potential type mismatches caused by
        // RTTI in a polymorphic scenario
        const std::type_info& type = (T_isPointer) ? typeid(T) : typeid(data);
        const int sz = sizeof(data);

        // for primitive types we are using our own type names instead of
        // using std:::type_info::name(), because the precise output of the
        // latter may vary between compilers
        if (type == typeid(char))
            // special case: C++ defines "char" to be neither type
            // equivalent to "signed char" nor to "unsigned char",
            // because e.g. char's signedness is implementation specific
            return DataType(T_isPointer, sz, "char");
        if (type == typeid(int8_t))   return DataType(T_isPointer, sz, "int8");
        if (type == typeid(uint8_t))  return DataType(T_isPointer, sz, "uint8");
        if (type == typeid(int16_t))  return DataType(T_isPointer, sz, "int16");
        if (type == typeid(uint16_t)) return DataType(T_isPointer, sz, "uint16");
        if (type == typeid(int32_t))  return DataType(T_isPointer, sz, "int32");
        if (type == typeid(uint32_t)) return DataType(T_isPointer, sz, "uint32");
        if (type == typeid(int64_t))  return DataType(T_isPointer, sz, "int64");
        if (type == typeid(uint64_t)) return DataType(T_isPointer, sz, "uint64");
        // CAUTION: depending on the system, "long" or "int" types may not
        // already been covered by the typedefs above
        if (type == typeid(long) || type == typeid(int)) {
            if (sz == 1) return DataType(T_isPointer, sz, "int8");
            if (sz == 2) return DataType(T_isPointer, sz, "int16");
            if (sz == 4) return DataType(T_isPointer, sz, "int32");
            if (sz == 8) return DataType(T_isPointer, sz, "int64");
            else assert(false /* unknown int / long size */);
        }
        if (type == typeid(unsigned long) || type == typeid(unsigned int)) {
            if (sz == 1) return DataType(T_isPointer, sz, "uint8");
            if (sz == 2) return DataType(T_isPointer, sz, "uint16");
            if (sz == 4) return DataType(T_isPointer, sz, "uint32");
            if (sz == 8) return DataType(T_isPointer, sz, "uint64");
            else assert(false /* unknown unsigned int / unsigned long size */);
        }
        if (type == typeid(bool))     return DataType(T_isPointer, sz, "bool");
        if (type == typeid(float))    return DataType(T_isPointer, sz, "real32");
        if (type == typeid(double))   return DataType(T_isPointer, sz, "real64");
        if (type == typeid(String))   return DataType(T_isPointer, sz, "String");

        // for pointers we must stick to pointer's compile-time declared
        // type and not use RTTI for the pointed object, because the
        // latter would easily create a type mismatch conflict for
        // pointers and objects in a polymorphic scenario, because the
        // pointer might have been declared with a base class type,
        // while actually assigned objects to these pointers might be of
        // any other derived class and RTTI would resolve the latter
        const String rawCppName =
            (T_isPointer) ? rawCppTypeName<T>()
                          : rawCppTypeNameOf(data);

        if (IsEnum(data)) return DataType(T_isPointer, sz, "enum", rawCppName);
        if (IsUnion(data)) return DataType(T_isPointer, sz, "union", rawCppName);
        if (IsClass(data)) return DataType(T_isPointer, sz, "class", rawCppName);

        assert(false /* unknown data type*/);
        return DataType();
    }

    /**
     * Will be thrown whenever an error occurs during an serialization or
     * deserialization process.
     */
    class Exception {
        public:
            String Message;

            Exception(String format, ...);
            Exception(String format, va_list arg);
            void PrintMessage() const;
            virtual ~Exception() {}

        protected:
            Exception();
            static String assemble(String format, va_list arg);
    };

} // namespace Serialization

#endif // LIBGIG_SERIALIZATION_H
