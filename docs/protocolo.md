# Protocolo JRP/1 (Job Relay Protocol) — borrador para discusión

Estado: **arquitectura cerrada en forma provisoria (2026-11-10)**. Prototipo
implementado el 2026-11-10 con autorización del dueño (ver README).

## 1. Decisiones del dueño (2026-11-08)

- Los datos se guardan **en memoria** siempre que sea posible.
- Todo dato es **binario opaco**: el servicio no conoce ni interpreta su formato.
- **El problema:** dentro de un mismo workflow run, a veces no se puede pasar
  información entre **steps** o entre **jobs** por los medios de GitHub, por tamaño u
  otro motivo (los outputs tienen tope de 1 MB por job y 50 MB por run). El relay guarda
  esa información para que otro step u otro job **del mismo run** la rescate.
- **Un mensaje se identifica por el run, el job y el step que lo produjeron.**
- **Los datos no sobreviven al workflow run.**
- Protocolo propio, con ideas de Protocol Buffers y de los protocolos sobre ZeroMQ.

## 2. El caso de uso

```
run 12345
  job "build"
    step "compile"  ──PUT "bin"──▶  [relay: (12345, build, compile, bin) → blob]
    step "package"  ──GET compile/bin──▶  recibe el blob (mismo job)
  job "test"        ──GET build/compile/bin──▶  recibe el blob (otro job)
  fin del run       ──▶  el relay libera todo lo del run 12345
```

Es el mismo modelo que los outputs de GitHub (`steps.<step>.outputs.<nombre>` dentro
del job, `needs.<job>.outputs.<nombre>` entre jobs), pero sin límite de tamaño y con
datos binarios.

- **Identidad del mensaje = (run_id, job_id, step_id, nombre).** Confirmada por el
  dueño el 2026-11-10.
  - `run_id`: lo fija el servidor desde el token del job. El cliente no lo elige.
  - `job_id`: el identificador del job en el archivo del workflow (`jobs.<job_id>`,
    disponible como `github.job`), el mismo que el consumidor usa en `needs:`.
  - `step_id`: el `id:` del step productor, el mismo que se usa en `steps.<id>`.
  - `nombre`: opcional, para que un step deje más de un mensaje (por defecto, `default`).
  - No se usan los IDs numéricos de la API: cambian en cada intento y el consumidor no
    los conoce de antemano.
- **Pedido abreviado:** dentro del mismo job, el consumidor puede omitir el `job_id`
  (se entiende el propio).
- **Reescritura:** si el step o job se re-ejecuta, su mensaje nuevo reemplaza al
  anterior al confirmarse.
- **Fin del run:** se libera todo. Si alguien usa "Re-run failed jobs" después de
  terminado el run, el consumidor recibe un error claro que sugiere "Re-run all jobs".

**Fuera de alcance por ahora:** traspaso entre runs o entre workflows, y jobs de matriz
(ver P2).

## 3. Qué se toma de Protocol Buffers

- **Varints** y **campos numerados** (`(número << 3) | tipo`) para los metadatos: son
  compactos y se pueden extender sin romper versiones. Los campos desconocidos se
  ignoran.
- **El contenido no va en protobuf.** Su guía advierte que no está diseñado para
  mensajes grandes (sobre ~1 MB conviene otra estrategia) y un campo `bytes` tiene
  límite de 2 GB. El contenido viaja crudo, en frames de ZeroMQ, sin copias.
- No se usa `protoc` ni la librería: solo la codificación de cable, que es pequeña.

## 4. Qué se toma de ZeroMQ

- **Frames de ZMTP 3.1**: cada mensaje JRP es un mensaje de varios frames; los frames
  largos admiten hasta 2^63−1 bytes.
- **CURVE + ZAP**: cifrado y control de quién se conecta.
- **Crédito en bytes, de FILEMQ**: quien recibe autoriza cuántos bytes acepta; quien
  envía no pasa de ahí. Protege la memoria de ambos lados.
- Sockets: servidor **ROUTER**, clientes **DEALER**.

## 5. Formato

```
Frame 1  Cabecera fija, 4 bytes: firma 0x4A 0x52 ("JR"), versión 0x01, comando
Frame 2  Metadatos TLV estilo protobuf (puede ir vacío)
Frame 3+ Datos crudos, un frame por trozo (solo en DATA)
```

| Código | Comando | Dirección | Metadatos |
|---|---|---|---|
| 0x01 | HELLO | C → S | token, job_id (el job que se conecta) |
| 0x02 | WELCOME | S → C | tamaño_total (máximo por mensaje), tamaño_trozo |
| 0x10 | PUT | C → S | job_id, step_id, nombre, tamaño_total |
| 0x11 | DATA | ambos | offset, fin |
| 0x12 | COMMIT | C → S | digest |
| 0x20 | GET | C → S | job_id (opcional), step_id, nombre, offset (para reanudar) |
| 0x21 | CREDIT | ambos | crédito |
| 0x22 | END | S → C | tamaño_total, digest |
| 0x30 | RELEASE | C → S | — (libera todos los mensajes del run; lo usa el paso final de limpieza) |
| 0x7E | OK | S → C | crédito inicial (tras PUT) |
| 0x7F | ERROR | S → C | código, texto |

| N.º | Campo | Tipo |
|---|---|---|
| 1 | job_id | LEN |
| 2 | tamaño_total | VARINT |
| 3 | offset | VARINT |
| 4 | crédito | VARINT |
| 5 | digest (BLAKE3-256) | LEN |
| 6 | fin | VARINT |
| 7 | código_error | VARINT |
| 8 | texto_error | LEN |
| 9 | token | LEN |
| 10 | step_id | LEN |
| 11 | nombre | LEN |
| 12 | tamaño_trozo | VARINT |

Reservados para más adelante, sin uso hoy: comandos 0x40–0x4F y campos 13–20.

**Regla del servidor:** en PUT, el `job_id` (si viene) debe ser el mismo del HELLO: un
job solo escribe sus propios mensajes. En GET, si no viene `job_id`, se entiende el
propio (pedido entre steps del mismo job).

## 6. Flujos

- **Subida (productor):** HELLO → PUT → OK(crédito) → DATA… → CREDIT… → COMMIT → OK.
  Al confirmar, el mensaje queda inmutable.
- **Descarga (consumidor):** HELLO → GET(build/compile/bin) + CREDIT → DATA… → CREDIT… → END.
  Si se corta, GET con el último offset recibido reanuda.

## 7. Memoria del servidor

- Índice: `run_id → { (job_id, step_id, nombre) → blob }`. Al terminar el run se borra
  la entrada completa.
- Un blob es una lista de trozos (los frames DATA tal como llegaron, movidos con
  `std::move`), no un buffer contiguo.
- Subida en curso: `std::unique_ptr` (un dueño). Al confirmar: `std::shared_ptr<const
  Blob>` (el índice y cada descarga en curso lo comparten; borrar el run no libera
  memoria que se está enviando).
- Tope de memoria configurable, revisado en el PUT con el tamaño declarado. Si no cabe,
  ERROR inmediato (D1). Los datos viven solo en memoria.
- Si el PUT no declara tamaño, el servidor corta con ERROR apenas la subida pasa el
  espacio disponible y descarta lo recibido.

## 8. Decisiones

Las marcadas "provisorio" fueron aprobadas por el dueño el 2026-11-10 para probar
("prueba así por mientras") y pueden cambiar con lo que se aprenda.

| # | Pregunta | Opciones |
|---|---|---|
| D1 | Memoria insuficiente | **Decidido (dueño, 2026-11-10): (a) rechazar el PUT con error claro.** El servidor revisa el tamaño declarado al inicio; si no cabe, el step falla de inmediato. Pasar a disco queda para una versión futura, sin romper el protocolo |
| D3 | ¿GET antes del COMMIT del productor? | Entre jobs, el consumidor depende del productor con `needs:`; entre steps, los steps corren en orden. En ambos casos el productor ya terminó. **Provisorio (dueño, 2026-11-10):** no se permite; se responde "no listo" |
| D2 | Reinicio del servidor | **Provisorio (dueño, 2026-11-10):** se pierde lo que estaba en memoria; el run afectado se re-ejecuta completo |
| D4 | Tamaño de trozo | **Provisorio:** 4 MiB por defecto, configurable |
| D5 | Integridad | **Provisorio:** un hash BLAKE3 del mensaje completo, al subir (COMMIT) y al bajar (END); sin hash por trozo |
| D6 | Autenticación | **Provisorio:** token OIDC de GitHub de cada job |
| D7 | Codificador de metadatos | **Provisorio:** escrito a mano |
| P1 | ¿Varios mensajes por step? | **Resuelta:** campo `nombre` opcional |
| P2 | ¿Jobs de matriz? | Comparten el mismo `job_id`, así que chocarían. Hoy fuera de alcance |
| P3 | ¿El token OIDC trae el job_id y el step_id? | **Respondida:** trae `run_id`, `run_attempt` y `check_run_id` (el job numérico de ese intento), pero no los ids del archivo. Propuesta: el servidor fija el run desde el token; `job_id` y `step_id` los declara el cliente dentro de su run, y el servidor guarda el `check_run_id` de quien escribió, para auditoría |

## Fuentes

- Protocol Buffers, codificación: https://protobuf.dev/programming-guides/encoding/
- Protocol Buffers, técnicas: https://protobuf.dev/programming-guides/techniques/
- 37/ZMTP 3.1: https://rfc.zeromq.org/spec/37/ · 25/ZMTP-CURVE: https://rfc.zeromq.org/spec/25/
- 19/FILEMQ: https://rfc.zeromq.org/spec/19/
- GitHub, pasar información entre jobs (límites de outputs): https://docs.github.com/en/enterprise-server@3.16/actions/how-tos/writing-workflows/choosing-what-your-workflow-does/passing-information-between-jobs
- GitHub, re-ejecutar jobs: https://docs.github.com/en/enterprise-server@3.14/actions/how-tos/manage-workflow-runs/re-run-workflows-and-jobs
- GitHub, claims del token OIDC: https://docs.github.com/enterprise-cloud@latest/actions/reference/security/oidc
- GitHub, límites de runners propios: https://docs.github.com/en/enterprise-server@3.14/actions/reference/usage-limits-for-self-hosted-runners
