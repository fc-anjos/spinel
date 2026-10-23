# A pattern that fails to compile raises RegexpError with the engine's
# message copied into a String. The copy's allocation can collect, and the
# handler used to have the engine's static message buffer registered as a
# root by then: the collector read the byte in front of it for a String
# header (make rt-san-check, under SPINEL_GC_STRESS=2, reports the read).
["[[:(a|#\\b)", "(", "a{2,1}", "[b-a]", "(?<n>a)\\k<m>"].each do |src|
  begin
    Regexp.new(src)
    puts "compiled"
  rescue RegexpError => e
    puts "RegexpError #{e.message.empty? ? 'without' : 'with'} a message"
  end
end
