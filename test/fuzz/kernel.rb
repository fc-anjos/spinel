# Fuzz kernel: one entry per runtime surface that takes untrusted bytes.
# Exported with --ext-entry and driven by harness.c under libFuzzer; a Ruby
# exception is an expected outcome (the harness catches it through the try
# helper), a sanitizer report or a crash is the finding.
module FuzzKernel
  def self.marshal(s)
    Marshal.load(s)
    0
  end

  # The format directive comes before the first newline, the packed bytes after.
  # A count of five digits or more is skipped: an integer directive with no
  # data left yields nil per count, in CRuby as here, so "q522343234" is a
  # correct half-billion-element Array and only the fuzzer's RSS limit.
  def self.unpack(s)
    i = s.index("\n")
    return 0 if i.nil?
    fmt = s[0, i]
    return 0 if fmt =~ /\d{5}/
    s[i + 1, s.length - i - 1].unpack(fmt).length
  end

  # The pattern comes before the first newline, the subject after.
  def self.regexp(s)
    i = s.index("\n")
    return 0 if i.nil?
    m = Regexp.new(s[0, i]).match(s[i + 1, s.length - i - 1])
    m.nil? ? 0 : 1
  end
end

# Types the entries' parameter as String; not run by the harness.
if __FILE__ == $0
  FuzzKernel.marshal("\x04\b0")
  FuzzKernel.unpack("C*\nab")
  FuzzKernel.regexp("a+\naa")
end
