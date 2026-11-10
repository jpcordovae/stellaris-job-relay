# stellaris-job-relay (nombre de trabajo)

Servicio de **Stellaris SpA** que corre en la red local de cada empresa y guarda,
en memoria, los datos que un step o un job necesita pasarle a otro step o job del
**mismo workflow run** de GitHub Actions, sin el tope de 1 MB por job y 50 MB por run
de los outputs de GitHub.

Protocolo propio (**JRP/1**) sobre ZeroMQ, en C++23. Diseño en
[docs/protocolo.md](docs/protocolo.md) y [docs/arquitectura.md](docs/arquitectura.md).

## Estado: prototipo

| Qué | Estado |
|---|---|
| Protocolo JRP/1 (codec varint/TLV, comandos, crédito) | Hecho |
| Servidor en memoria, rechazo si no cabe (D1), liberación del run | Hecho |
| Cliente `jrelay` (put / get / release / keygen) | Hecho |
| Cifrado CURVE (opcional) | Hecho |
| **Verificación del token OIDC de GitHub (D6)** | **Pendiente.** Hoy solo hay `--dev-auth`, inseguro |
| Liberación automática consultando la API de GitHub | Pendiente (hoy: paso `jrelay release`) |
| Jobs de matriz, traspaso entre runs | Fuera de alcance |

## Uso en un workflow

Ver [examples/workflow.yml](examples/workflow.yml). Cada mensaje se identifica por
**(run, job, step, nombre)**, como los outputs de GitHub:

```bash
# step "compile" del job "build"
jrelay put --name app.tar --file dist/app.tar
# un step posterior del mismo job
jrelay get --step compile --name app.tar --out app.tar
# otro job del mismo run
jrelay get --job build --step compile --name app.tar --out app.tar
# al final del run
jrelay release
```

Códigos de salida: 0 bien, 1 error, 2 uso, 3 no existe, 4 el run ya terminó,
5 no cabe en la memoria del relay.

## Compilar y probar

Requiere CMake ≥ 3.25, GCC 13 o superior (Clang 18 con la librería estándar de GCC 13 no
habilita `std::expected`; usar Clang 19 o superior), `libzmq3-dev`, `cppzmq-dev` y
`doctest-dev`. BLAKE3 se descarga de su repositorio oficial al configurar.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/jrelay_tests                   # 30 pruebas: codec, almacén y extremo a extremo
bench/bench.sh build 1024              # simula un run con 1 GiB y mide memoria y velocidad

# con AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DJRELAY_SANITIZE=ON && cmake --build build-asan -j
./build-asan/jrelay_tests
```

## Mediciones del prototipo (2026-11-10)

Una máquina de 2 vCPU y 8 GB, todo por loopback, mensaje de 1 GiB aleatorio:

| Operación | Velocidad |
|---|---|
| put (step "compile") | ~1.300 MiB/s |
| get, mismo job | ~1.500 MiB/s |
| 2 get simultáneos, otro job | ~960 y ~660 MiB/s |
| **Memoria máxima del servidor** | **1.030 MiB** para un mensaje de 1.024 MiB, durante 3 descargas (2 simultáneas): las descargas comparten los datos, no los copian |

En una red real la velocidad la pondrá la red; lo importante es que el servidor no
agrega copias.
