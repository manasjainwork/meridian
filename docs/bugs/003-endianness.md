# 003: The htons bug, caught by tests

## The bug
A uint16_t book_type = 1 written in host order goes on the wire as 01 00.
A big-endian reader sees 0x0100 = 256 and rejects the order.
No crash, no exception, just a silent rejection.

## Three layers of defence
1. Compile time: BE<T> is a distinct type, a raw integer cannot be assigned to a wire field by accident.
2. Property test: 1,000,000 random values, every byte on the wire checked against the expected byte.
3. Golden bytes: a fixed 38-byte NewOrder must match byte for byte.

## Mutation test
I replaced bswap16 with an identity function. Both test_endianness and
test_protocol failed immediately. Restored with git checkout, all green.

## Known hole
BE<uint16_t> x{1} (brace init) writes the raw value without swapping. Fix later with a private constructor.
