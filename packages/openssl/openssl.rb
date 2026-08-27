# Spinel bundled `openssl` -- the SSL half only.
#
# The spelling is CRuby's, deliberately: the goal is that a program written
# against CRuby compiles here, so inventing a shorter name would be worth
# nothing. What is missing is missing the way a subset is missing things --
# OpenSSL::Digest, Cipher, PKey and most of X509 are not here, and a program
# that uses them fails at COMPILE time with an unresolved call rather than at
# run time somewhere deep.
#
# Spinel implements no TLS. sp_openssl.c is glue over the system libssl, and
# the trust anchors are the operating system's; nothing is bundled.
#
# SSLSocket is not an IO, exactly as in CRuby: it is an ordinary object that
# answers #to_io, which is why IO.select had to learn that protocol. The
# handle is an Integer naming a connection in the C table -- no SSL pointer
# is handed to a garbage-collected world.
require "openssl/buffering"
require "openssl/digest"

module OpenSSL
  module Native
    native_lib "openssl"
    native_obj "packages/openssl/sp_openssl.o"
    native_func :connect,      [:int, :string, :int], :int,    "sp_ssl_connect"
    native_func :read,         [:int, :int],          :string, "sp_ssl_read"
    native_func :write,        [:int, :string, :int], :int,    "sp_ssl_write"
    native_func :pending,      [:int],                :int,    "sp_ssl_pending"
    native_func :close,        [:int],                :int,    "sp_ssl_close"
    native_func :last_error,   [],                    :string, "sp_ssl_last_error"
    native_func :peer_subject, [:int],                :string, "sp_ssl_peer_subject"
    native_func :version,      [:int],                :string, "sp_ssl_version"
    native_func :cipher,       [:int],                :string, "sp_ssl_cipher"
    native_func :read_nb,      [:int, :int],          :string, "sp_ssl_read_nb"
    native_func :want,         [],                    :int,    "sp_ssl_want"
    native_func :write_nb,     [:int, :string, :int], :int,    "sp_ssl_write_nb"
    ffi_lib "ssl"
    ffi_lib "crypto"
  end

  module SSL
    VERIFY_NONE = 0
    VERIFY_PEER = 1

    class SSLError < OpenSSLError
    end

    # CRuby raises these, not an SSLError extended with a module at run time.
    # A non-blocking TLS read can need the socket to become WRITABLE before it
    # can read -- a renegotiating peer does that -- so the two directions are
    # separate classes and the caller waits on the right one:
    #
    #   begin
    #     data = ssl.read_nonblock(4096)
    #   rescue IO::WaitReadable
    #     IO.select([ssl]); retry
    #   rescue IO::WaitWritable
    #     IO.select(nil, [ssl]); retry
    #   end
    class SSLErrorWaitReadable < SSLError
      include IO::WaitReadable
    end

    class SSLErrorWaitWritable < SSLError
      include IO::WaitWritable
    end

    # Only the members an outbound HTTPS client reaches. set_params is the one
    # Net::HTTP calls; the rest of CRuby's forty-odd accessors are not here.
    class SSLContext
      attr_accessor :verify_mode
      attr_accessor :verify_hostname

      def initialize
        @verify_mode = VERIFY_PEER
        @verify_hostname = true
      end

      def set_params(params = nil)
        @verify_mode = VERIFY_PEER
        @verify_hostname = true
        self
      end
    end

    class SSLSocket
      # Not an IO: an ordinary object with the buffered surface mixed in and
      # the handle behind #to_io, exactly as CRuby has it.
      include OpenSSL::Buffering

      attr_accessor :hostname

      def initialize(io, context = nil)
        super()
        @io = io
        @context = context.nil? ? SSLContext.new : context
        @handle = -1
        @hostname = ""
      end

      def context
        @context
      end

      # CRuby's SSLSocket#to_io answers the underlying socket, and every
      # forwarder (fileno, addr, closed?) goes through it. IO.select uses it
      # to find the descriptor to wait on.
      def to_io
        @io
      end

      def connect
        h = Native.connect(@io.fileno, @hostname,
                           @context.verify_mode == VERIFY_NONE ? 0 : 1)
        if h < 0
          raise SSLError, "SSL_connect returned an error: #{Native.last_error}"
        end
        @handle = h
        self
      end

      def sysread(maxlen)
        raise SSLError, "not connected" if @handle < 0
        s = Native.read(@handle, maxlen)
        s.empty? ? nil : s
      end

      def syswrite(data)
        raise SSLError, "not connected" if @handle < 0
        n = Native.write(@handle, data, data.bytesize)
        raise SSLError, "SSL_write returned an error: #{Native.last_error}" if n < 0
        n
      end

      # One record-layer read that never blocks. The descriptor must already
      # be non-blocking -- CRuby says the same -- and this does not set it,
      # because the fd belongs to the IO the caller handed over.
      #
      # `exception: false` answers :wait_readable / :wait_writable instead of
      # raising, and nil at EOF, which is CRuby's contract for the same call.
      def sysread_nonblock(maxlen, exception: true)
        raise SSLError, "not connected" if @handle < 0
        return "" if maxlen == 0
        s = Native.read_nb(@handle, maxlen)
        return s unless s.empty?
        case Native.want
        when 1
          return :wait_readable unless exception
          raise SSLErrorWaitReadable, "read would block"
        when 2
          return :wait_writable unless exception
          raise SSLErrorWaitWritable, "write would block"
        when 3
          return nil unless exception
          raise EOFError, "end of file reached"
        else
          raise SSLError, "SSL_read returned an error: #{Native.last_error}"
        end
      end

      # The mirror of sysread_nonblock. A TLS write can need the socket to
      # become READABLE before it can write -- the peer's side of a
      # renegotiation -- so this raises the other class too, and a caller that
      # waits on the wrong direction waits forever.
      def syswrite_nonblock(data, exception: true)
        raise SSLError, "not connected" if @handle < 0
        s = data.to_s
        return 0 if s.empty?
        n = Native.write_nb(@handle, s, s.bytesize)
        return n if n > 0
        case Native.want
        when 1
          return :wait_readable unless exception
          raise SSLErrorWaitReadable, "read would block"
        when 2
          return :wait_writable unless exception
          raise SSLErrorWaitWritable, "write would block"
        when 3
          return nil unless exception
          raise EOFError, "end of file reached"
        else
          raise SSLError, "SSL_write returned an error: #{Native.last_error}"
        end
      end

      # Bytes already decrypted and waiting inside the record layer. An event
      # loop that waits on the descriptor alone will not see these: a whole
      # record can arrive in one read, leaving the fd quiet while the
      # application still has data to take. CRuby has the same trap and the
      # same escape hatch.
      def pending
        @handle < 0 ? 0 : Native.pending(@handle)
      end

      def sysclose
        return nil if @handle < 0
        Native.close(@handle)
        @handle = -1
        nil
      end

      def peer_subject
        @handle < 0 ? "" : Native.peer_subject(@handle)
      end

      def ssl_version
        @handle < 0 ? "" : Native.version(@handle)
      end

      def cipher_name
        @handle < 0 ? "" : Native.cipher(@handle)
      end
    end
  end
end
