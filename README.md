# stellaris-job-relay (nombre de trabajo)

Servicio de **Stellaris SpA** que se ejecuta en la red local de cada empresa cliente y
permite transferir datos entre jobs de GitHub Actions (y a futuro otros CI/CD) sin los
límites de outputs, artifacts y caché de la plataforma.

Implementación prevista: **C++ moderno** sobre **ZeroMQ** (libzmq / cppzmq).

## Estado

**Fase de arquitectura. No hay código todavía, a propósito.**
La discusión vive en [docs/arquitectura.md](docs/arquitectura.md) y las decisiones
cerradas en [docs/adr/](docs/adr/).
