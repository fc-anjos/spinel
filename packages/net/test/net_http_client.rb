# The HTTP client against a server in this same program, so the test is
# deterministic and needs no network. Every line is diffed against CRuby's own
# net/http: the request line and Host header a server actually sees, the
# response parse, Content-Length and chunked bodies, POST with a form body,
# and the response class family that `res.is_a?(Net::HTTPSuccess)` needs.
#
# The https path cannot be here -- it needs a peer with a certificate -- and is
# exercised by hand against a real host; see the package README note in
# net/http.rb.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]

t = Thread.new do
  seen = []
  8.times do
    c = server.accept
    req = c.gets.to_s.strip                 # the request line
    headers = {}
    raw = []
    while (line = c.gets)
      line = line.strip
      break if line.empty?
      raw << line
      ci = line.index(":")
      headers[line[0, ci].downcase] = line[(ci + 1)..-1].to_s.strip unless ci.nil?
    end
    body = ""
    if headers.key?("content-length")
      n = headers["content-length"].to_i
      body = c.read(n).to_s if n > 0
    end
    # The port is random, so record whether Host carried it rather than its value.
    hostok = headers["host"] == "127.0.0.1:#{port}"
    # `headers` is keyed downcased, so two spellings of one name collapse
    # here; count the raw lines instead or a duplicate goes unseen.
    ctypes = raw.count { |l| l.downcase.start_with?("content-type:") }
    multi = raw.find { |l| l.downcase.start_with?("x-multi:") }.to_s
    seen << "#{req}|host-has-port=#{hostok}|content-type-lines=#{ctypes}|#{multi}|body=#{body}"

    if req.include?("/chunked")
      c.write("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
      c.write("5\r\nhello\r\n")
      c.write("6\r\n world\r\n")
      c.write("0\r\n\r\n")
    elsif req.include?("/notfound")
      c.write("HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\nConnection: close\r\n\r\nbye")
    else
      payload = "ok:#{body}"
      c.write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n" \
              "Content-Length: #{payload.bytesize}\r\nConnection: close\r\n\r\n#{payload}")
    end
    c.close
  end
  seen
end

# get_response through a URI, the idiom every caller writes.
res = Net::HTTP.get_response(URI("http://127.0.0.1:#{port}/a?x=1"))
p [res.code, res.message, res.body]
p res["content-type"]
p res["CONTENT-TYPE"]          # header lookup is case-insensitive
p res.is_a?(Net::HTTPSuccess)
p res.is_a?(Net::HTTPOK)

# The block form, with an explicit request object.
Net::HTTP.start("127.0.0.1", port) do |http|
  r = http.request(Net::HTTP::Get.new("/chunked"))
  p [r.code, r.body]           # chunked body is reassembled
end

# POST with a form body.
r3 = Net::HTTP.post_form(URI("http://127.0.0.1:#{port}/f"), { "a" => "1", "b" => "x y" })
p [r3.code, r3.body]

# A non-2xx is a response, not an exception -- as CRuby has it.
r4 = Net::HTTP.get_response(URI("http://127.0.0.1:#{port}/notfound"))
p [r4.code, r4.message, r4.body]
p [r4.is_a?(Net::HTTPNotFound), r4.is_a?(Net::HTTPClientError), r4.is_a?(Net::HTTPSuccess)]

# Several requests inside one `start` block. Every request goes out with
# `Connection: close`, so the socket the second would use is dead; CRuby
# reconnects transparently there and so does this. One connection per request
# rather than keep-alive -- what it must not be is a failure on the second.
Net::HTTP.start("127.0.0.1", port) do |http|
  p http.get("/one").body
  p http.get("/two").body
end

# A request object built from a URI with an initial header hash, sent through a
# Net::HTTP that was never started -- both are CRuby spellings, and together
# they are how a caller that already parsed the URL writes a POST.
uri = URI("http://127.0.0.1:#{port}/hook?k=v")
post = Net::HTTP::Post.new(uri, "Content-Type" => "application/json")
post.body = "{}"
p post.path                    # the URI's request_uri, not its whole to_s
p post["content-type"]
r5 = Net::HTTP.new(uri.host, uri.port).request(post)
p [r5.code, r5.body]

# A header set twice under two spellings is ONE header. `initheader` takes the
# caller's lowercase spelling, `content_type=` then overwrites it; sending both
# leaves the server to pick, which is not a choice it should have.
uri2 = URI("http://127.0.0.1:#{port}/dup")
dup = Net::HTTP::Post.new(uri2, "content-type" => "text/plain")
dup.content_type = "application/json"
p dup["Content-Type"]
p dup["content-type"]          # either spelling finds the one value
# A multi-valued header is ONE line, joined with ", ". `to_s` on an Array is
# its inspect form, so getting this wrong puts `["a", "b"]` on the wire. Set
# through `#[]=`, which is the path CRuby supports for an Array: its
# initheader half calls `value.strip` per value and raises NoMethodError.
# Folded onto this request rather than a fresh one on purpose -- a request
# built WITHOUT the optional initheader, beside these that have it, is the
# arity pair that segfaults (#4134).
dup["X-Multi"] = [ "one", "two" ]
p dup["X-Multi"]
dup.body = "{}"
r6 = Net::HTTP.new(uri2.host, uri2.port).request(dup)
p r6.code

t.value.each { |line| puts line }
server.close
