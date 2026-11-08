# Protocolo JRP/1 (Job Relay Protocol) — borrador para discusión

Estado: **propuesta**. Nada se implementa hasta que el dueño lo apruebe.

## 1. Decisiones del dueño (2026-11-08)

- Los datos se guardan **en memoria** siempre que sea posible.
- Todo dato es **binario opaco**: el servicio no conoce ni interpreta su formato.
- **Un solo caso de uso por ahora:** mensajes entre jobs de un **mismo workflow run**.
- **Un mensaje se identifica por el run y el job que lo produjo.**
- **Los datos no sobreviven al workflow run.**
- Protocolo propio, con ideas de Protocol Buffers y de los protocolos sobre ZeroMQ.

## 2. El caso de uso

```
run 12345
  job "build"  ──PUT──▶  [relay: (12345, "build") → blob]
  job "test"   ──GET "build"──▶  recibe el blob
  job "deploy" ──GET "build"──▶  recibe el mismo blob
  fin del run  ──▶  el relay libera todo lo del run 12345
```

- **Identidad del mensaje = (run_id, job_id del productor).**
  - `run_id`: lo fija el servidor desde el token del job. El cliente no lo elige.
  - `job_id`: el identificador del job en el archivo del workflow (`jobs.<job_id>`,
    disponible como `github.job`). Es el mismo nombre que el consumidor ya usa en
    `needs:`, así que sabe a quién pedirle.
  - No se usa el ID numérico del job de la API: cambia en cada intento y el consumidor
    no lo conoce de antemano.
- **Un mensaje por job productor.** Si el job se re-ejecuta, su mensaje nuevo reemplaza
  al anterior al confirmarse.
- **Fin del run:** se libera todo. Si alguien usa "Re-run failed jobs" después de
  terminado el run, el consumidor recibe un error claro que sugiere "Re-run all jobs".

**Fuera de alcance por ahora:** traspaso entre runs o entre workflows, varios mensajes
por job, y jobs de matriz (ver P1 y P2).

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
| 0x01 | HELLO | C → S | token |
| 0x02 | WELCOME | S → C | tamaño máximo, trozo sugerido |
| 0x10 | PUT | C → S | job_id, tamaño_total |
| 0x11 | DATA | ambos | offset, fin |
| 0x12 | COMMIT | C → S | digest |
| 0x20 | GET | C → S | job_id, offset (para reanudar) |
| 0x21 | CREDIT | ambos | crédito |
| 0x22 | END | S → C | digest |
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

Reservados para más adelante, sin uso hoy: comandos 0x40–0x4F y campos 10–20.

## 6. Flujos

- **Subida (productor):** HELLO → PUT → OK(crédito) → DATA… → CREDIT… → COMMIT → OK.
  Al confirmar, el mensaje queda inmutable.
- **Descarga (consumidor):** HELLO → GET("build") + CREDIT → DATA… → CREDIT… → END.
  Si se corta, GET con el último offset recibido reanuda.

## 7. Memoria del servidor

- Índice: `run_id → { job_id → blob }`. Al terminar el run se borra la entrada completa.
- Un blob es una lista de trozos (los frames DATA tal como llegaron, movidos con
  `std::move`), no un buffer contiguo.
- Subida en curso: `std::unique_ptr` (un dueño). Al confirmar: `std::shared_ptr<const
  Blob>` (el índice y cada descarga en curso lo comparten; borrar el run no libera
  memoria que se está enviando).
- Tope de memoria configurable, revisado en el PUT con el tamaño declarado (D1).

## 8. Preguntas abiertas

| # | Pregunta | Opciones |
|---|---|---|
| D1 | Memoria insuficiente | (a) rechazar el PUT con error claro; (b) pasar a disco; (c) según configuración |
| D3 | ¿GET antes del COMMIT del productor? | En el caso de uso actual el consumidor depende del productor con `needs:`, así que empieza cuando el productor terminó. Recomendado: no permitirlo; responder "no listo" |
| P1 | ¿Varios mensajes por job? | Hoy uno. Si hace falta, agregar un nombre de mensaje opcional (campo 10) |
| P2 | ¿Jobs de matriz? | Comparten el mismo `job_id`, así que chocarían. Hoy fuera de alcance |
| P3 | ¿El token OIDC trae el job_id? | **Respondida:** trae `run_id`, `run_attempt` y `check_run_id` (el job numérico de ese intento), pero no el `job_id` del archivo. Propuesta: el servidor fija el run desde el token; el `job_id` lo declara el cliente dentro de su run, y el servidor guarda el `check_run_id` de quien escribió, para auditoría |

## Fuentes

- Protocol Buffers, codificación: https://protobuf.dev/programming-guides/encoding/
- Protocol Buffers, técnicas: https://protobuf.dev/programming-guides/techniques/
- 37/ZMTP 3.1: https://rfc.zeromq.org/spec/37/ · 25/ZMTP-CURVE: https://rfc.zeromq.org/spec/25/
- 19/FILEMQ: https://rfc.zeromq.org/spec/19/
- GitHub, re-ejecutar jobs: https://docs.github.com/en/enterprise-server@3.14/actions/how-tos/manage-workflow-runs/re-run-workflows-and-jobs
- GitHub, claims del token OIDC: https://docs.github.com/enterprise-cloud@latest/actions/reference/security/oidc
- GitHub, límites de runners propios: https://docs.github.com/en/enterprise-server@3.14/actions/reference/usage-limits-for-self-hosted-runners
