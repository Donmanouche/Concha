// StrudelDirt's effect modules on its instruments (tools/sc/compare_strudeldirt.py)
stack(
  s("superpiano").note("c3 e3").squiz("<2 4>").waveloss(20),
  s("superfm").note("g2").krush(0.5).kcutoff(2000).triode(0.3),
  s("supersquare").note("a2").octer(0.5).octersub(0.3).ring(0.5).ringf(200),
  s("superhammond").note("e3").fshift(100).fshiftnote(1).distort2(0.4),
  s("superpiano").note("c4").psrate(2).psdisp(0.1).tilt(0.5).plat(0.2),
  s("supervibe").note("d3").tremolorate(4).tremolodepth(0.5),
  s("superpiano").note("c3").comb(0.3).smear(0.2).freeze(0).binshift(1.5),
  s("superchip").note("f3").hbrick(0.2).lbrick(0.1).real(0.5).imag(0.2).enhance(0.3).tsdelay(0.5).xsdelay(0.5)
).slow(2)
