# Copyright (C) 2012  Internet Systems Consortium.
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SYSTEMS CONSORTIUM
# DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
# INTERNET SYSTEMS CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT,
# INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING
# FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
# NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
# WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

import unittest
from pydnspp import *

class NSEC3HashTest(unittest.TestCase):
    '''These tests are mostly straightforward conversion of C++ tests
    except for python specific type checks.

    '''

    def setUp(self):
        self.test_hash = NSEC3Hash(Rdata(RRType.NSEC3PARAM(), RRClass.IN(),
                                         "1 0 12 aabbccdd"))

    def test_bad_construct(self):
        # missing parameter
        self.assertRaises(TypeError, NSEC3Hash)

        # invalid type of argument
        self.assertRaises(TypeError, NSEC3Hash, "1 0 12 aabbccdd")

        # additional parameter
        self.assertRaises(TypeError, NSEC3Hash, Rdata(RRType.NSEC3PARAM(),
                                                      RRClass.IN(),
                                                      "1 0 12 aabbccdd"), 1)

    def test_unknown_algorithm(self):
        self.assertRaises(UnknownNSEC3HashAlgorithm, NSEC3Hash,
                          Rdata(RRType.NSEC3PARAM(), RRClass.IN(),
                                "2 0 12 aabbccdd"))

    def test_calculate(self):
        # A couple of normal cases from the RFC5155 example.
        self.assertEqual("0P9MHAVEQVM6T7VBL5LOP2U3T2RP3TOM",
                         self.test_hash.calculate(Name("example")))
        self.assertEqual("35MTHGPGCU1QG68FAB165KLNSNK3DPVL",
                         self.test_hash.calculate(Name("a.example")))

        # Check case-insensitiveness
        self.assertEqual("0P9MHAVEQVM6T7VBL5LOP2U3T2RP3TOM",
                         self.test_hash.calculate(Name("EXAMPLE")))

        # Some boundary cases: 0-iteration and empty salt.  Borrowed from the
        # .com zone data.
        self.test_hash = NSEC3Hash(Rdata(RRType.NSEC3PARAM(),
                                         RRClass.IN(),"1 0 0 -"))
        self.assertEqual("CK0POJMG874LJREF7EFN8430QVIT8BSM",
                         self.test_hash.calculate(Name("com")))

        # Using unusually large iterations, something larger than the 8-bit
        #range.  (expected hash value generated by BIND 9's dnssec-signzone)
        self.test_hash = NSEC3Hash(Rdata(RRType.NSEC3PARAM(),
                                         RRClass.IN(), "1 0 256 AABBCCDD"))
        self.assertEqual("COG6A52MJ96MNMV3QUCAGGCO0RHCC2Q3",
                         self.test_hash.calculate(Name("example.org")))

    def test_calculate_badparam(self):
        self.assertRaises(TypeError, self.test_hash.calculate, "example")
        self.assertRaises(TypeError, self.test_hash.calculate)
        self.assertRaises(TypeError, self.test_hash.calculate, Name("."), 1)

if __name__ == '__main__':
    unittest.main()