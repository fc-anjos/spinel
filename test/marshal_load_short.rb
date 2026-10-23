# A stream shorter than a record's own length or count claims is "marshal
# data too short", raised before anything is sized from the claim: a String
# claiming 2**31 - 1 bytes in an 8-byte stream used to allocate the claim
# and read zeros past the end for all of it.
[
  "\x04\b\"\x04\xff\xff\xff\x7f",   # String, length 2**31 - 1
  "\x04\b\"\nabc",                  # String, length 5, 3 bytes given
  "\x04\b:\nab",                    # Symbol, length 5, 2 bytes given
  "\x04\bl+\x04\xff\xff\xff\x7f",   # Bignum, 2**31 - 1 words
  "\x04\b[\x07i\x06",               # Array of 2, 1 element given
  "\x04\b{\x06i\x06",               # Hash of 1 pair, key only
  "\x04\bi",                        # Integer, no bytes
].each do |s|
  begin
    p Marshal.load(s)
  rescue ArgumentError => e
    puts "ArgumentError: #{e.message}"
  end
end
p Marshal.load("\x04\b\"\x08abc")
