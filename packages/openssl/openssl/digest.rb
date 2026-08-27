# OpenSSL::Digest and OpenSSL::HMAC -- the names a CRuby program writes for a
# hash or a signature, over the hashes the runtime already carries
# (lib/sp_crypto.c, the same implementations the `digest` package binds).
# Nothing here reaches libssl: these are not the TLS surface, they are the
# other half of the OpenSSL namespace that programs name, and there is no
# reason to route them through a second implementation.
#
# Subset, and it is the same one `digest` has: the class-method forms only.
# The incremental object API (`d = OpenSSL::Digest::SHA256.new; d << part`) is
# not modelled, and neither is OpenSSL::Digest.new("SHA256") -- an algorithm
# chosen by a runtime string cannot resolve to a C function at compile time.
module OpenSSL
  # CRuby's base for everything in this namespace; SSLError and DigestError
  # both descend from it, so `rescue OpenSSL::OpenSSLError` catches either.
  class OpenSSLError < StandardError
  end

  module Crypto
    native_lib "openssl"
    native_func :sha256_hex,      [:string],          :cstring, "sp_crypto_sha256_hex"
    native_func :sha256_bin,      [:string],          :cbinstr, "sp_crypto_sha256_bin"
    native_func :sha1_hex,        [:string],          :cstring, "sp_crypto_sha1_hex"
    native_func :sha1_bin,        [:string],          :cbinstr, "sp_crypto_sha1_bin"
    native_func :hmac_sha256_hex, [:string, :string], :cstring, "sp_crypto_hmac_sha256_hex"
    native_func :hmac_sha1_hex,   [:string, :string], :cstring, "sp_crypto_hmac_sha1_hex"
  end

  module Digest
    class DigestError < OpenSSLError
    end

    module SHA256
      def self.hexdigest(data) = Crypto.sha256_hex(data)
      def self.digest(data)    = Crypto.sha256_bin(data)
    end

    module SHA1
      def self.hexdigest(data) = Crypto.sha1_hex(data)
      def self.digest(data)    = Crypto.sha1_bin(data)
    end
  end

  module HMAC
    # CRuby takes the algorithm first, as a String or a Digest instance. Only
    # the String form is here, and only for the two the runtime carries: an
    # algorithm that is not one of them raises DigestError -- the class CRuby
    # raises, so `rescue OpenSSL::Digest::DigestError` catches the same thing
    # -- rather than answering a hash from the wrong function. MD5 is one of
    # those: CRuby has it, the runtime's crypto does not.
    def self.hexdigest(algo, key, data)
      case algo.to_s.upcase
      when "SHA256" then Crypto.hmac_sha256_hex(key, data)
      when "SHA1"   then Crypto.hmac_sha1_hex(key, data)
      else raise Digest::DigestError, "unsupported digest algorithm: #{algo}"
      end
    end
  end
end
