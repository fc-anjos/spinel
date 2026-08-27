# Spinel bundled `uri` -- the generic/HTTP/HTTPS half.
#
# The spelling is CRuby's. What is here is what an HTTP client reaches:
# `URI(str)`, the component readers, #request_uri and #to_s. What is not here
# is absent the way a subset is absent things, and a program naming it fails
# to compile rather than at run time:
#
# * only http, https and a bare/relative form parse; there is no URI::FTP,
#   URI::LDAP, URI::MailTo, URI::WS, or the scheme registry behind them
# * userinfo, fragment and opaque are parsed and readable, but there is no
#   #merge, #route_to, #normalize or #select
# * URI.encode_www_form_component / .decode_www_form_component are here
#   because a query builder needs them; the URI.escape family CRuby removed
#   is not
module URI
  class Error < StandardError
  end

  class InvalidURIError < Error
  end

  class Generic
    attr_reader :scheme, :userinfo, :host, :port, :path, :query, :fragment

    def initialize(scheme, userinfo, host, port, path, query, fragment)
      @scheme = scheme
      @userinfo = userinfo
      @host = host
      @port = port
      @path = path
      @query = query
      @fragment = fragment
    end

    def default_port
      case @scheme
      when "https" then 443
      when "http" then 80
      else 0
      end
    end

    # What goes on the request line: the path with the query, and "/" when the
    # path is empty -- `URI("http://example.com").request_uri` is "/".
    def request_uri
      p = (@path.nil? || @path.empty?) ? "/" : @path
      @query.nil? || @query.empty? ? p : "#{p}?#{@query}"
    end

    def hostname
      @host
    end

    def to_s
      s = String.new
      s << "#{@scheme}://" unless @scheme.nil? || @scheme.empty?
      s << "#{@userinfo}@" unless @userinfo.nil? || @userinfo.empty?
      s << @host.to_s
      s << ":#{@port}" if @port != default_port && @port > 0
      s << @path.to_s
      s << "?#{@query}" unless @query.nil? || @query.empty?
      s << "##{@fragment}" unless @fragment.nil? || @fragment.empty?
      s
    end

    def inspect
      "#<#{self.class}: #{self}>"
    end

    def ==(other)
      other.is_a?(Generic) && to_s == other.to_s
    end
  end

  class HTTP < Generic
  end

  class HTTPS < HTTP
  end

  # Percent-encode one www-form component: everything but the unreserved set,
  # with a space as "+", which is what CRuby does here (and what differs from
  # a path escape).
  def self.encode_www_form_component(str)
    out = String.new
    str.to_s.each_char do |ch|
      if ch =~ /\A[A-Za-z0-9\*\-\.\_]\z/
        out << ch
      elsif ch == " "
        out << "+"
      else
        ch.bytes.each { |b| out << format("%%%02X", b) }
      end
    end
    out
  end

  def self.decode_www_form_component(str)
    out = String.new
    s = str.to_s
    i = 0
    while i < s.length
      ch = s[i]
      if ch == "+"
        out << " "
        i += 1
      elsif ch == "%" && i + 2 < s.length
        out << s[i + 1, 2].to_i(16).chr
        i += 3
      else
        out << ch
        i += 1
      end
    end
    out
  end

  # `URI.encode_www_form({"a" => 1, "b" => "x y"})` -> "a=1&b=x+y"
  def self.encode_www_form(pairs)
    parts = []
    pairs.each do |k, v|
      parts << "#{encode_www_form_component(k)}=#{encode_www_form_component(v)}"
    end
    parts.join("&")
  end

  def self.parse(str)
    s = str.to_s
    scheme = ""
    rest = s
    idx = s.index("://")
    if idx
      scheme = s[0, idx].downcase
      rest = s[(idx + 3)..-1].to_s
    end

    fragment = ""
    fi = rest.index("#")
    if fi
      fragment = rest[(fi + 1)..-1].to_s
      rest = rest[0, fi]
    end

    query = ""
    qi = rest.index("?")
    if qi
      query = rest[(qi + 1)..-1].to_s
      rest = rest[0, qi]
    end

    authority = rest
    path = ""
    pi = rest.index("/")
    if pi
      authority = rest[0, pi]
      path = rest[pi..-1].to_s
    end

    userinfo = ""
    ai = authority.index("@")
    if ai
      userinfo = authority[0, ai]
      authority = authority[(ai + 1)..-1].to_s
    end

    host = authority
    port = 0
    ci = authority.rindex(":")
    if ci && !authority[(ci + 1)..-1].to_s.empty? &&
       authority[(ci + 1)..-1].to_s =~ /\A[0-9]+\z/
      host = authority[0, ci]
      port = authority[(ci + 1)..-1].to_i
    end

    if port == 0
      port = 443 if scheme == "https"
      port = 80 if scheme == "http"
    end

    if scheme == "https"
      HTTPS.new(scheme, userinfo, host, port, path, query, fragment)
    elsif scheme == "http"
      HTTP.new(scheme, userinfo, host, port, path, query, fragment)
    else
      Generic.new(scheme, userinfo, host, port, path, query, fragment)
    end
  end

  def self.join(base, rel)
    b = parse(base.to_s)
    r = rel.to_s
    return parse(r) if r.include?("://")
    if r.start_with?("/")
      path = r
    else
      dir = b.path.to_s
      cut = dir.rindex("/")
      dir = cut ? dir[0, cut + 1] : "/"
      path = dir + r
    end
    q = ""
    qi = path.index("?")
    if qi
      q = path[(qi + 1)..-1].to_s
      path = path[0, qi]
    end
    if b.scheme == "https"
      HTTPS.new(b.scheme, b.userinfo, b.host, b.port, path, q, "")
    else
      HTTP.new(b.scheme, b.userinfo, b.host, b.port, path, q, "")
    end
  end
end

# `URI("https://example.com/x")` -- the Kernel method every caller writes.
def URI(str)
  str.is_a?(URI::Generic) ? str : URI.parse(str)
end
