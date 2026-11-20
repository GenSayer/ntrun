// eh.cpp: C++ exceptions: types, destructors on the way, rethrow, nested
#include <stdio.h>

struct Tr {
    const char *n;
    Tr(const char *s) : n(s) { printf("ctor %s\n", n); }
    ~Tr() { printf("dtor %s\n", n); }
};

struct Base {
    virtual const char *what() const { return "Base"; }
    virtual ~Base() {}
};

struct Derived : Base {
    const char *what() const { return "Derived"; }
};

static void thrower(int k)
{
    Tr t("thrower");
    if (k == 1) throw 42;
    if (k == 2) throw Derived();
    if (k == 3) throw "text";
    if (k == 4) throw 2.5;
}

static void middle(int k)
{
    Tr t("middle");
    thrower(k);
    printf("middle returns\n");
}

static void rethrower()
{
    try {
        middle(1);
    } catch (int e) {
        printf("rethrow %d\n", e);
        throw;
    }
}

int main()
{
    int k;
    for (k = 0; k <= 4; k++) {
        try {
            middle(k);
        } catch (int e) {
            printf("int %d\n", e);
        } catch (Base &b) {
            printf("class %s\n", b.what());
        } catch (const char *s) {
            printf("str %s\n", s);
        } catch (...) {
            printf("other\n");
        }
    }
    try {
        rethrower();
    } catch (int e) {
        printf("caught again %d\n", e);
    }
    try {
        try {
            throw 1;
        } catch (int) {
            throw 2;
        }
    } catch (int e) {
        printf("nested %d\n", e);
    }
    printf("done\n");
    return 0;
}
