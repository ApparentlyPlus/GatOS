curl https://ftp.gnu.org/gnu/xorriso/xorriso-1.5.2.tar.gz
tar xzf xorriso-1.5.2.tar.gz
cd xorriso-1.5.2.tar.gz
./configure --prefix=/usr
make
strip ./xorriso/xorriso