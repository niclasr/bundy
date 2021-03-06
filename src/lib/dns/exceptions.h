// Copyright (C) 2010  Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

// XXX: we have another exceptions.h, so we need to use a prefix "DNS_" in
// the include guard.  More preferably, we should define a consistent naming
// style for the header guide (e.g. module-name_file-name_H) throughout the
// package.

#ifndef DNS_EXCEPTIONS_H
#define DNS_EXCEPTIONS_H 1

#include <exceptions/exceptions.h>

namespace bundy {
namespace dns {

///
/// \brief A standard DNS module exception ...[TBD]
///
class Rcode;                    // forward declaration

class Exception : public bundy::Exception {
public:
    Exception(const char* file, size_t line, const char* what) :
        bundy::Exception(file, line, what) {}
};

///
/// \brief Base class for all sorts of text parse errors.
///
class DNSTextError : public bundy::dns::Exception {
public:
    DNSTextError(const char* file, size_t line, const char* what) :
        bundy::dns::Exception(file, line, what) {}
};

///
/// \brief Base class for name parser exceptions.
///
class NameParserException : public DNSTextError {
public:
    NameParserException(const char* file, size_t line, const char* what) :
        DNSTextError(file, line, what) {}
};

class DNSProtocolError : public bundy::dns::Exception {
public:
    DNSProtocolError(const char* file, size_t line, const char* what) :
        bundy::dns::Exception(file, line, what) {}
    virtual const Rcode& getRcode() const = 0;
};

class DNSMessageFORMERR : public DNSProtocolError {
public:
    DNSMessageFORMERR(const char* file, size_t line, const char* what) :
        DNSProtocolError(file, line, what) {}
    virtual const Rcode& getRcode() const;
};

class DNSMessageBADVERS : public DNSProtocolError {
public:
    DNSMessageBADVERS(const char* file, size_t line, const char* what) :
        DNSProtocolError(file, line, what) {}
    virtual const Rcode& getRcode() const;
};

}
}
#endif  // DNS_EXCEPTIONS_H

// Local Variables: 
// mode: c++
// End: 
