# An --ext library whose kernel uses Marshal: the symbol interner it hands
# the Marshal reader is exported, and its forward declaration has to agree.
module ExtMarshal
  def self.load_size(s)
    Marshal.load(s).to_s.length
  end
end

if __FILE__ == $0
  ExtMarshal.load_size("\x04\b0")
end
