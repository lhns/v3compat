// A shared library with an lzcnt in it, loaded at run time (the client has to see code mapped after start).
unsigned long lz(unsigned long x) {
    unsigned long r;
    __asm__("lzcnt %1, %0" : "=r"(r) : "r"(x));
    return r;
}
