/* Negative link check: the kernel has native TLS, but no ELF TLS loader. */
thread_local int tls_value = 7;
int main() { return ++tls_value; }
