#!/bin/bash --
cd keyicons
#for X in `seq 48 57` `seq 65 90`; do
#  pngff < charset.png | ff-text 16 16 `dc -e $X"P"` | ffxbm X > $X.xbm
#done
sed 's/^  //' <<"EOF" > ../keyicons.xbm
  #define keyicons_width 16
  #define keyicons_height 4096
  static unsigned char keyicons_bits[] = {
EOF
for X in `seq 0 255`; do
  test -f $X.xbm
  X=`dc -e "1 $? - $X *p"`
  sed -n 's/};/,/;/^ /p' < $X.xbm >> ../keyicons.xbm
done
echo '};' >> ../keyicons.xbm
