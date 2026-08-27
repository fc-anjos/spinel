# URI parsing and the component readers, diffed against CRuby: every line here
# answers what CRuby's URI answers, including the corners a client depends on
# -- request_uri is "/" for an empty path, the default port comes from the
# scheme and is left off #to_s, and www-form encoding turns a space into "+".
require "uri"
u = URI("https://example.com:8443/a/b?q=1#frag")
p [u.scheme, u.host, u.port, u.path, u.query, u.fragment]
p u.request_uri
p u.to_s
p URI("http://example.com/").port
p URI("https://example.com").request_uri
p URI("http://user:pw@example.com/x").userinfo
p URI.encode_www_form_component("a b&c=d")
p URI.decode_www_form_component("a+b%26c%3Dd")
p URI.encode_www_form({"a" => 1, "b" => "x y"})
p URI.join("https://example.com/a/b", "c").to_s
p URI.join("https://example.com/a/b", "/z").to_s
p URI("https://example.com/x") == URI("https://example.com/x")
