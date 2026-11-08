# Arquitectura — borrador para discusión

Nada de este documento está decidido. Cada punto se cierra con un ADR en `docs/adr/`.

## Problema

GitHub Actions limita el traspaso de datos entre jobs (outputs pequeños; artifacts y
caché con cuotas y latencia). Los equipos con pipelines pesados (binarios, datasets,
imágenes) necesitan mover gigabytes entre jobs, rápido y dentro de su propia red.

## Restricciones ya conocidas

1. **Los jobs no son simultáneos.** El consumidor suele arrancar después de que el
   productor terminó. ZeroMQ es transporte, no almacenamiento, así que el servicio
   necesita persistencia (store-and-forward) o los datos se pierden.
2. **Requiere runners self-hosted** en la red del cliente para llegar al servicio local.
3. **Seguridad:** el servicio recibirá artefactos de build de terceros. Requiere
   autenticación por job/workflow y cifrado en tránsito.

## Preguntas abiertas

### Modelo y protocolo
- [ ] ¿Modelo de datos: blobs por clave (`run_id/job/nombre`) o streams?
- [ ] ¿Patrón ZeroMQ: ROUTER/DEALER con broker, o productor/consumidor directo si
      coinciden en el tiempo?
- [ ] ¿Transferencia en chunks con reanudación? ¿Tamaño de chunk?
- [ ] ¿Integridad: hash por chunk y por blob (BLAKE3 / SHA-256)?

### Persistencia
- [x] **Decidido (dueño, 2026-11-08):** los datos se guardan en memoria siempre que sea
      posible, como binario opaco sin interpretar su formato, con un protocolo propio.
      Propuesta en [protocolo.md](protocolo.md).
- [ ] ¿Qué hacer cuando la memoria no alcanza o el servidor se reinicia? (D1 y D2 en
      protocolo.md)
- [ ] ¿Política de retención y expiración (TTL por run)?
- [ ] ¿Un nodo, o varios con replicación?

### Seguridad
- [ ] ¿CurveZMQ para cifrado y autenticación de clientes?
- [ ] ¿Cómo se obtiene la credencial del job: OIDC token de GitHub verificado por el
      servicio, o secretos estáticos?
- [ ] ¿Aislamiento entre repos/organizaciones en una misma instalación?

### Integración
- [ ] ¿Forma de uso en el workflow: GitHub Action propia (`uses: stellaris/relay@v1`),
      CLI en el runner, o ambas?
- [ ] ¿Soporte futuro para GitLab CI / Jenkins?

### Implementación C++
- [ ] Estándar: ¿C++23 (requiere GCC 13+/Clang 17+)?
- [ ] Propiedad de memoria: `unique_ptr` + `std::move` por defecto; `shared_ptr`
      solo con propiedad realmente compartida (su contador es atómico y tiene costo)
- [ ] Zero-copy de buffers con `zmq::message_t`
- [ ] Manejo de errores: ¿`std::expected` en vez de excepciones en la ruta caliente?
- [ ] ¿Modelo de concurrencia: hilos de I/O de ZeroMQ + pool, o corrutinas C++20?
- [ ] Build y dependencias: CMake + vcpkg o Conan; sanitizers y fuzzing en CI
- [ ] Empaquetado: binario estático, paquete .deb/.rpm, contenedor

### Producto
- [ ] Licencia: ¿open source (core) + versión comercial, o propietaria?
- [ ] Nombre definitivo
