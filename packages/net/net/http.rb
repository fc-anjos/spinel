# Spinel bundled `net/http` -- an HTTP/HTTPS client.
#
# The spelling is CRuby's, so a program written against it compiles here:
#
#     Net::HTTP.get(URI("https://example.com/"))
#     res = Net::HTTP.get_response(URI("https://example.com/"))
#     Net::HTTP.start(host, 443, use_ssl: true) { |http| http.request(req) }
#
# Pure Ruby over TCPSocket and the openssl package: nothing here speaks TLS,
# it asks OpenSSL::SSL::SSLSocket for a connection and then reads and writes
# lines. Which also means an https request needs the openssl package, and says
# so if it is missing rather than failing somewhere lower down.
#
# What is absent is absent the way a subset is, and a program naming it fails
# to compile rather than at run time:
#
# * HTTP/1.1 with `Connection: close`, one request per connection. There is no
#   keep-alive, no pipelining and no HTTP/2
# * no proxy support, no cookie jar, no automatic redirect following
#   (#get_response hands back the 3xx and its Location, as CRuby's does)
# * no streaming body block on #request; the body is read whole
# * chunked transfer decoding is here; content-encoding (gzip) is not
require "socket"
require "uri"

module Net
  class HTTPError < StandardError
  end

  # The response. `code` is a String, as CRuby has it ("200", not 200), and
  # header lookup through #[] is case-insensitive.
  class HTTPResponse
    attr_reader :http_version, :code, :message, :body

    def initialize(http_version, code, message, headers, body)
      @http_version = http_version
      @code = code
      @message = message
      @headers = headers          # downcased name => value
      @body = body
    end

    def [](name)
      @headers[name.to_s.downcase]
    end

    def key?(name)
      @headers.key?(name.to_s.downcase)
    end

    def each_header
      @headers.each { |k, v| yield k, v }
      nil
    end

    def content_type
      @headers["content-type"]
    end
  end

  # The response family. CRuby's success test is `res.is_a?(Net::HTTPSuccess)`
  # rather than a predicate on the code, so the classes have to exist for that
  # line to compile. The families are all here; of the per-code classes only
  # the ones a client actually names are, which is the usual subset rule.
  class HTTPInformation < HTTPResponse; end
  class HTTPSuccess < HTTPResponse; end
  class HTTPRedirection < HTTPResponse; end
  class HTTPClientError < HTTPResponse; end
  class HTTPServerError < HTTPResponse; end

  class HTTPOK < HTTPSuccess; end
  class HTTPCreated < HTTPSuccess; end
  class HTTPNoContent < HTTPSuccess; end
  class HTTPMovedPermanently < HTTPRedirection; end
  class HTTPFound < HTTPRedirection; end
  class HTTPBadRequest < HTTPClientError; end
  class HTTPUnauthorized < HTTPClientError; end
  class HTTPForbidden < HTTPClientError; end
  class HTTPNotFound < HTTPClientError; end
  class HTTPInternalServerError < HTTPServerError; end

  # A request. CRuby builds these as Net::HTTP::Get.new(path) and friends;
  # the same shape is here so the same code compiles.
  class HTTPRequest
    attr_reader :method, :path
    attr_accessor :body

    # `path` is a request path, or a URI -- CRuby takes either, and
    # `Net::HTTP::Post.new(uri, "Content-Type" => "application/json")` is the
    # spelling a caller reaches for when it already parsed the URL. A URI
    # contributes its `request_uri` (the path with the query), not its `to_s`,
    # which is the whole URL and would go out as an absolute request line.
    def initialize(method, path, initheader = nil)
      @method = method
      @path = if path.is_a?(URI::Generic)
        path.request_uri
      else
        (path.nil? || path.to_s.empty?) ? "/" : path.to_s
      end
      # Downcased name => value, with the caller's spelling kept beside it for
      # the wire. One key per header name makes a duplicate impossible to
      # construct, which is what "keep the spelling, scan for case variants on
      # every write" was doing by hand. The response half of this file has
      # stored them downcased all along.
      @headers = {}
      @header_names = {}
      @body = ""
      initheader.each { |k, v| self[k] = v } unless initheader.nil?
    end

    # Header names are case-insensitive on the wire, and CRuby's
    # Net::HTTPHeader is case-insensitive on both halves -- as the response
    # half here already is. What is kept, rather than downcased the way the
    # response stores them, is the caller's spelling: for a request that is
    # what goes out on the wire.
    # Header names are case-insensitive on the wire, and CRuby's
    # Net::HTTPHeader is case-insensitive on both halves. Storing under the
    # downcased name makes that a lookup rather than a scan, and makes it
    # impossible to hold one header twice under two spellings -- which is what
    # `Post.new(uri, "content-type" => …)` followed by `req.content_type = …`
    # used to do, sending both and leaving the server to pick.
    def []=(name, value)
      k = name.to_s.downcase
      # An Array joins with ", ", the way CRuby serves a multi-valued header:
      # `req["Accept"] = %w[a b]` reads back "a, b" and goes out as one line.
      # `to_s` on an Array is its INSPECT form, so without this the wire got
      # `Accept: ["a", "b"]`.
      #
      # CRuby only reaches that path through `#[]=`; its initheader half calls
      # `value.strip` per value and so raises NoMethodError for an Array. This
      # package routes initheader through `#[]=` and therefore accepts one -- a
      # deliberate divergence, in the permissive direction, and one rule for one
      # value shape rather than two.
      @headers[k] = value.is_a?(Array) ? value.map { |v| v.to_s }.join(", ") : value.to_s
      @header_names[k] = name.to_s
      value
    end

    def [](name)
      @headers[name.to_s.downcase]
    end

    def key?(name)
      @headers.key?(name.to_s.downcase)
    end

    # Yields the spelling the caller wrote, not the downcased key: for a
    # request that is what goes out on the wire.
    def each_header
      @headers.each do |k, v|
        name = @header_names[k]
        yield (name.nil? ? k : name), v
      end
      nil
    end

    def content_type=(v)
      self["Content-Type"] = v
    end

    def set_form_data(hash)
      @body = URI.encode_www_form(hash)
      self["Content-Type"] = "application/x-www-form-urlencoded"
      @body
    end
  end

  class HTTP
    # CRuby builds requests as Net::HTTP::Get.new(path) and friends. Same
    # spelling here, so the same code compiles.
    class Get < HTTPRequest
      def initialize(path, initheader = nil)
        super("GET", path, initheader)
      end
    end

    class Post < HTTPRequest
      def initialize(path, initheader = nil)
        super("POST", path, initheader)
      end
    end

    class Put < HTTPRequest
      def initialize(path, initheader = nil)
        super("PUT", path, initheader)
      end
    end

    class Delete < HTTPRequest
      def initialize(path, initheader = nil)
        super("DELETE", path, initheader)
      end
    end

    class Head < HTTPRequest
      def initialize(path, initheader = nil)
        super("HEAD", path, initheader)
      end
    end

    attr_reader :address, :port
    attr_accessor :use_ssl, :open_timeout, :read_timeout

    def initialize(address, port = 80)
      @address = address
      @port = port
      @use_ssl = false
      @open_timeout = 60
      @read_timeout = 60
      @socket = nil
      @tls = nil
      @fresh = false
      @started = false
    end

    def use_ssl?
      @use_ssl
    end

    def started?
      @started
    end

    # Net::HTTP.start(host, port, use_ssl: true) { |http| ... }
    def self.start(address, port = 80, use_ssl: false)
      http = HTTP.new(address, port)
      http.use_ssl = use_ssl
      http.start
      begin
        yield http
      ensure
        http.finish
      end
    end

    # Net::HTTP.get(uri) -> the body String.
    def self.get(uri)
      get_response(uri).body
    end

    # Net::HTTP.get_response(uri) -> HTTPResponse.
    def self.get_response(uri)
      u = URI(uri)
      https = u.scheme == "https"
      http = HTTP.new(u.host, u.port)
      http.use_ssl = https
      http.start
      begin
        http.request(HTTPRequest.new("GET", u.request_uri))
      ensure
        http.finish
      end
    end

    def self.post_form(uri, params)
      u = URI(uri)
      req = HTTPRequest.new("POST", u.request_uri)
      req.set_form_data(params)
      http = HTTP.new(u.host, u.port)
      http.use_ssl = (u.scheme == "https")
      http.start
      begin
        http.request(req)
      ensure
        http.finish
      end
    end

    def start
      open_connection
      @started = true
      self
    end

    def open_connection
      @socket = TCPSocket.new(@address, @port)
      if @use_ssl
        tls = OpenSSL::SSL::SSLSocket.new(@socket)
        tls.hostname = @address
        tls.connect
        @tls = tls
      end
      @fresh = true
      nil
    end

    def finish
      @tls.sysclose unless @tls.nil?
      @socket.close unless @socket.nil?
      @tls = nil
      @socket = nil
      @fresh = false
      @started = false
      nil
    end

    def get(path, headers = nil)
      req = HTTPRequest.new("GET", path)
      headers.each { |k, v| req[k] = v } unless headers.nil?
      request(req)
    end

    def post(path, body, headers = nil)
      req = HTTPRequest.new("POST", path)
      req.body = body
      headers.each { |k, v| req[k] = v } unless headers.nil?
      request(req)
    end

    def request(req)
      # CRuby opens a connection for a #request on an unstarted Net::HTTP, runs
      # the request over it and closes it again -- so `Net::HTTP.new(host,
      # port).request(req)` works without a `start`, and is the spelling a
      # caller writes when the connection is configured somewhere other than
      # where the request is sent. Doing that here rather than raising keeps
      # the two spellings interchangeable, as they are there.
      unless @started
        # `start` is INSIDE the begin: `open_connection` assigns @socket and
        # only then completes the TLS handshake, so a handshake failure raises
        # with a live socket that nothing else will close. `finish` is a no-op
        # when there is nothing open, which is the other way start can fail.
        begin
          start
          return request(req)
        ensure
          finish
        end
      end
      # Every request goes out with `Connection: close`, so the server hangs
      # up after answering and the socket a second request would use is dead.
      # CRuby reconnects transparently in that situation, and a caller writing
      # `start { |http| http.get("/a"); http.get("/b") }` -- which is the
      # idiom -- has no reason to know. This is one connection per request
      # rather than keep-alive; what it is not is a failure on the second one.
      reconnect unless @fresh
      @fresh = false
      write_request(req)
      read_response
    end

    def reconnect
      @tls.sysclose unless @tls.nil?
      @socket.close unless @socket.nil?
      @tls = nil
      @socket = nil
      open_connection
    end

    # ---- the wire ----

    def wire_write(s)
      if @tls.nil?
        @socket.write(s)
      else
        @tls.write(s)
      end
      nil
    end

    def wire_gets
      @tls.nil? ? @socket.gets : @tls.gets
    end

    def wire_read(n)
      @tls.nil? ? @socket.read(n) : @tls.read(n)
    end

    def wire_read_all
      @tls.nil? ? @socket.read : @tls.read
    end

    def write_request(req)
      out = String.new
      out << "#{req.method} #{req.path} HTTP/1.1\r\n"
      # The port belongs in Host unless it is the scheme's default, which is
      # what a virtual host on a non-standard port depends on.
      default = @use_ssl ? 443 : 80
      out << (@port == default ? "Host: #{@address}\r\n" : "Host: #{@address}:#{@port}\r\n")
      have_len = false
      req.each_header do |k, v|
        have_len = true if k.downcase == "content-length"
        out << "#{k}: #{v}\r\n"
      end
      body = req.body.to_s
      out << "Content-Length: #{body.bytesize}\r\n" if !have_len && !body.empty?
      out << "Connection: close\r\n"
      out << "\r\n"
      out << body
      wire_write(out)
    end

    def read_response
      status = wire_gets
      raise HTTPError, "no response from #{@address}" if status.nil?
      parts = status.strip.split(" ")
      version = parts[0].to_s
      code = parts.length > 1 ? parts[1].to_s : ""
      message = parts.length > 2 ? parts[2..-1].join(" ") : ""

      headers = {}
      while (line = wire_gets)
        line = line.strip
        break if line.empty?
        ci = line.index(":")
        next if ci.nil?
        headers[line[0, ci].downcase] = line[(ci + 1)..-1].to_s.strip
      end

      body =
        if headers["transfer-encoding"].to_s.downcase == "chunked"
          read_chunked
        elsif headers.key?("content-length")
          n = headers["content-length"].to_i
          n > 0 ? wire_read(n).to_s : ""
        else
          wire_read_all.to_s
        end

      build_response(version, code, message, headers, body)
    end

    # The class a status code names. Specific first, then the family by its
    # leading digit, so an unlisted 2xx is still a Net::HTTPSuccess and
    # `res.is_a?(Net::HTTPSuccess)` answers what it should.
    def build_response(version, code, message, headers, body)
      case code
      when "200" then return HTTPOK.new(version, code, message, headers, body)
      when "201" then return HTTPCreated.new(version, code, message, headers, body)
      when "204" then return HTTPNoContent.new(version, code, message, headers, body)
      when "301" then return HTTPMovedPermanently.new(version, code, message, headers, body)
      when "302" then return HTTPFound.new(version, code, message, headers, body)
      when "400" then return HTTPBadRequest.new(version, code, message, headers, body)
      when "401" then return HTTPUnauthorized.new(version, code, message, headers, body)
      when "403" then return HTTPForbidden.new(version, code, message, headers, body)
      when "404" then return HTTPNotFound.new(version, code, message, headers, body)
      when "500" then return HTTPInternalServerError.new(version, code, message, headers, body)
      end
      case code[0, 1]
      when "1" then HTTPInformation.new(version, code, message, headers, body)
      when "2" then HTTPSuccess.new(version, code, message, headers, body)
      when "3" then HTTPRedirection.new(version, code, message, headers, body)
      when "4" then HTTPClientError.new(version, code, message, headers, body)
      when "5" then HTTPServerError.new(version, code, message, headers, body)
      else HTTPResponse.new(version, code, message, headers, body)
      end
    end

    # Chunked transfer: each chunk is a hex length line, the bytes, then CRLF.
    # A zero length ends it, and the trailer that may follow is read to the
    # blank line so the connection is left where the caller expects.
    def read_chunked
      out = String.new
      loop do
        line = wire_gets
        break if line.nil?
        size = line.strip.split(";")[0].to_s.to_i(16)
        if size == 0
          while (t = wire_gets)
            break if t.strip.empty?
          end
          break
        end
        chunk = wire_read(size)
        break if chunk.nil?
        out << chunk
        wire_gets                    # the CRLF after the chunk
      end
      out
    end
  end
end
