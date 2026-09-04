for f in src/*.c src/*.h include/*.h; do
    [ -f "$f" ] || continue
    echo "=== FILE: $f ==="
    cat "$f"
    echo ""
done > codebase.txt
