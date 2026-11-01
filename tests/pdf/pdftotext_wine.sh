# pdftotext_wine.cpp'nin host tarafı: pdftotext'i çalıştırır, sonucu <taban>.{out,err,done} dosyalarına yazar.
# Kullanım: sh pdftotext_wine.sh <taban> <pdftotext> [argümanlar...]
# .done en son ve atomik oluşur: Windows tarafı onu görünce diğer ikisi tamamdır.
base=$1
shift
"$@" >"$base.out" 2>"$base.err"
printf '%s' "$?" >"$base.tmp"
mv "$base.tmp" "$base.done"
