# `if obj.attr` had its live branch dropped when the backing ivar's only
# VISIBLE write was the constructor's nil and the real value arrived through a
# generated setter. emit_if folds a branch away when every program-wide write
# to the ivar assigns nil, but attr_writer / attr_accessor synthesize the
# setter, so `c.w = [0.5]` is a CallNode against a body that has no AST and the
# scan never saw it. The program took the else branch forever, silently.
# (matz/spinel#4107)
class Ctx
  attr_accessor :w
  def initialize
    @w = nil
  end
end

def probe(c)
  if c.w
    "truthy"
  else
    "falsy"
  end
end

c = Ctx.new
c.w = [0.5, 0.25]
puts probe(c)
# The same read reported the value as present all along, which is what made the
# bad branch silent rather than loud.
puts c.w.nil?
puts c.w.is_a?(Array)

# The other entry point: a generated writer plus a direct @w read inside the
# class, which reaches the fold through static_nil_ivar_cond instead.
class Direct
  attr_writer :v
  def initialize
    @v = nil
  end
  def check
    if @v
      "set"
    else
      "unset"
    end
  end
end

d = Direct.new
d.v = 7
puts d.check

# A Struct's members are generated writers too.
Point = Struct.new(:x)
pt = Point.new(nil)
pt.x = 3
puts(pt.x ? "struct-set" : "struct-unset")

# A writer reached through the superclass counts as generated as well.
class Base
  attr_accessor :z
end
class Derived < Base
  def initialize
    @z = nil
  end
  def check
    if @z
      "inherited-set"
    else
      "inherited-unset"
    end
  end
end
dd = Derived.new
dd.z = "here"
puts dd.check

# A hand-written setter is the boundary on the other side: its body holds a
# real write node, which the scan already counts, so nothing about the fold
# changes for it. (This case comes from bartleusink's #4108, which found the
# same bug and drew the boundary in both directions.)
class HandWritten
  attr_reader :h
  def initialize
    @h = nil
  end
  def h=(v)
    @h = v
  end
end
hw = HandWritten.new
hw.h = [2.0]
puts(hw.h ? "hand-written-set" : "hand-written-unset")

# The fold itself is still wanted: with no writer of any kind, an ivar written
# only nil really is statically falsy, and the branch really is dead.
class NoWriter
  attr_reader :m
  def initialize
    @m = nil
  end
end
nw = NoWriter.new
puts(nw.m ? "unreachable" : "correctly-falsy")
