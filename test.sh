echo "=== BASIC ==="
pwd
echo hello

echo "=== CD ==="
mkdir -p sub
cd sub
pwd
cd ..
cd no_dir

echo "=== BUILTINS ==="
which echo
which cd
which nope123

echo "=== REDIR ==="
echo test > out.txt
cat out.txt
pwd > dir.txt
cat dir.txt

echo "=== PIPES ==="
echo abc | cat
pwd | cat
ls | nope123

echo "=== PIPE + REDIR ==="
echo hi > f.txt
cat < f.txt | wc -l
ls | sort > sorted.txt
cat sorted.txt

echo "=== CONDITIONALS ==="
echo ok
and echo ok2
cd no_dir
and echo no
gibberish_cmd
or echo recovered

echo "=== BUILTINS IN PIPE ==="
pwd | cat
which ls | cat

echo "=== EXIT ==="
echo done
exit
