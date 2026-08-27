# OpenSSL::Digest and OpenSSL::HMAC over the runtime's own crypto. Every hash
# below is diffed against CRuby's, which is the point: these must be the same
# bytes, not merely the same shape.
#
# The last case is a subset boundary rather than a diff: CRuby carries MD5 and
# the runtime's crypto does not, so it raises where CRuby answers. It raises
# the class CRuby raises for an algorithm it does not have, so a program that
# rescues OpenSSL::Digest::DigestError catches the same thing.
require "openssl"

p OpenSSL::Digest::SHA256.hexdigest("hi")
p OpenSSL::Digest::SHA1.hexdigest("hi")
p OpenSSL::Digest::SHA256.hexdigest("")
p OpenSSL::Digest::SHA256.digest("hi").bytesize
p OpenSSL::Digest::SHA1.digest("hi").bytesize
p OpenSSL::HMAC.hexdigest("SHA256", "key", "msg")
p OpenSSL::HMAC.hexdigest("sha256", "key", "msg")
p OpenSSL::HMAC.hexdigest("SHA1", "key", "msg")
# A binary payload: the length rides the header, so an embedded NUL survives.
p OpenSSL::Digest::SHA256.hexdigest(String.new("a\0b"))

# The error hierarchy is CRuby's: DigestError and SSLError share a base.
p OpenSSL::Digest::DigestError.ancestors.take(3).map(&:to_s)
p OpenSSL::SSL::SSLError.ancestors.take(3).map(&:to_s)

begin
  OpenSSL::HMAC.hexdigest("MD5", "k", "m")
rescue OpenSSL::OpenSSLError => e
  puts "#{e.class}: #{e.message}"
end
