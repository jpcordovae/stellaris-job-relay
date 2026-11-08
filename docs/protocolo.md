# Protocolo JRP/1 (Job Relay Protocol) — borrador para discusión

Estado: **propuesta**. Nada se implementa hasta que el dueño lo apruebe.

## 1. Decisiones del dueño que este documento asume (2026-11-08)

- Los datos se guardan **en memoria** siempre que sea posible.
- Todo dato es **binario opaco**: el servicio no conoce ni interpreta el formato original.
- Se define un **protocolo propio**, tomando ideas de Protocol Buffers y de los
  protocolos construidos sobre ZeroMQ.

## 2. Qué tomar de Protocol Buffers (y qué no)

### Se toma

| Idea | Por qué sirve aquí |
|---|---|
| **Varints** (enteros de 1 a 10 bytes; los valores chicos ocupan poco) | Tamaños, offsets y créditos suelen ser chicos en metadatos y a veces enormes; un solo tipo cubre ambos casos |
| **Mensaje = pares clave-valor con etiqueta** `(número_de_campo << 3) \| tipo_de_cable` | Los metadatos son extensibles sin romper versiones |
| **Tipos de cable mínimos**: VARINT (0), I64 (1), LEN (2) | Basta para todo lo que el servicio necesita saber |
| **Campos desconocidos se ignoran** y los números de campo nunca se reutilizan | Un cliente nuevo puede hablar con un servidor viejo y al revés |

### No se toma

- **No se usa protobuf para el contenido.** Su propia guía dice que no está diseñado
  para mensajes grandes y sugiere otra estrategia sobre ~1 MB por mensaje; además, un
  campo `bytes` tiene límite de 2 GB y el mensaje se parsea completo en memoria. El
  contenido viaja crudo, en frames de ZeroMQ, sin copias.
- **No se agrega longitud propia al mensaje.** Protobuf no se auto-delimita y suele
  necesitar un prefijo de tamaño; ZMTP ya delimita cada frame, así que sería duplicar.
- **No se depende de `protoc` ni de la librería de protobuf.** Los metadatos usan solo
  la codificación de cable (varint + TLV), que se implementa en pocas líneas y se
  puede probar exhaustivamente.

## 3. Qué tomar de ZeroMQ

| Fuente | Idea | Uso en JRP/1 |
|---|---|---|
| ZMTP 3.1 (RFC 37) | Mensajes de varios frames con bandera MORE; frames largos con tamaño de 8 bytes (hasta 2^63−1) | Cabecera, metadatos y datos van en frames separados del mismo mensaje |
| ZMTP-CURVE (RFC 25) + ZAP | Cifrado y autenticación de pares en el transporte | Todo tráfico cifrado; ZAP decide quién se conecta |
| FILEMQ (RFC 19/35) | **Control de flujo por crédito en bytes**: el receptor da crédito y el emisor no envía más de lo autorizado; trozos con offset y marca de fin | Protege la memoria del servidor y del cliente; permite reanudar desde un offset |
| zguide, cap. 7 (transferencia de archivos, modelo 3) | Pedidos en tubería (*pipelining*) con un número fijo de trozos en vuelo | Rendimiento alto sin saturar colas de ROUTER/DEALER |
| FILEMQ, MDP | Firma fija al inicio de cada mensaje y un byte de comando | Rechazo inmediato de tráfico que no es JRP |

Los "command frames" de ZMTP no se usan: pertenecen a la capa de seguridad de libzmq y
la API no los expone. JRP/1 vive en frames de mensaje normales.

## 4. Formato de un mensaje JRP/1

Patrón de sockets: servidor **ROUTER**, clientes **DEALER** (asíncrono, permite varios
pedidos en vuelo por conexión). En el servidor, ROUTER antepone el frame de identidad.

```
Frame 1  Cabecera fija (8 bytes)
         0..1  firma      0x4A 0x52 ("JR")
         2     versión    0x01
         3     comando    (tabla 4.1)
         4..7  id_pedido  uint32 little-endian, lo elige el cliente; la respuesta lo repite
Frame 2  Metadatos: secuencia TLV estilo protobuf (puede ir vacío)
Frame 3+ Datos: bytes crudos, opacos, un frame por trozo (solo en DATA)
```

### 4.1 Comandos

| Código | Comando | Dirección | Para qué |
|---|---|---|---|
| 0x01 | HELLO | C → S | Presenta el token del job (identidad del workflow) |
| 0x02 | WELCOME | S → C | Acepta; informa límites (tamaño máximo, trozo sugerido) |
| 0x10 | PUT | C → S | Abre una subida: clave, tamaño total si se conoce, TTL |
| 0x11 | DATA | ambos | Trozo de datos con su offset |
| 0x12 | COMMIT | C → S | Cierra la subida con el digest del blob completo |
| 0x13 | ABORT | ambos | Cancela una subida o descarga en curso |
| 0x20 | GET | C → S | Pide una clave desde un offset (permite reanudar) |
| 0x21 | CREDIT | ambos | Autoriza N bytes más de DATA |
| 0x22 | END | S → C | Fin de la descarga, con el digest para verificar |
| 0x30 | STAT | C → S | Existe / tamaño / estado / vencimiento de una clave |
| 0x31 | DELETE | C → S | Borra una clave antes de su vencimiento |
| 0x7E | OK | S → C | Respuesta positiva genérica |
| 0x7F | ERROR | S → C | Código y texto de error |

### 4.2 Campos de metadatos (números fijos para siempre)

| N.º | Nombre | Tipo de cable | Uso |
|---|---|---|---|
| 1 | clave | LEN | Nombre lógico dentro del espacio del run (ver §6) |
| 2 | tamaño_total | VARINT | Bytes del blob, si se conoce de antemano |
| 3 | offset | VARINT | Posición del trozo o desde dónde reanudar |
| 4 | crédito | VARINT | Bytes autorizados |
| 5 | ttl_s | VARINT | Vida del blob en segundos |
| 6 | digest | LEN | Hash del blob completo |
| 7 | alg_digest | VARINT | 1 = BLAKE3-256 (otros en el futuro) |
| 8 | fin | VARINT | 1 = último trozo |
| 9 | codigo_error | VARINT | Tabla de errores (por definir) |
| 10 | texto_error | LEN | Mensaje para humanos |
| 11 | token | LEN | Solo en HELLO |
| 12 | etiqueta_cliente | LEN | Texto libre del cliente; **el servidor nunca lo interpreta** |

## 5. Flujos

**Subida:** PUT → OK con crédito inicial → DATA… (sin pasar el crédito) → CREDIT del
servidor a medida que guarda → … → COMMIT con digest → OK. Al confirmar, el blob queda
**inmutable**.

**Descarga:** GET (clave, offset) + CREDIT → DATA… → el cliente renueva CREDIT → END
con digest. Si se corta, un GET nuevo con el último offset recibido reanuda.

## 6. Modelo de memoria del servidor

- **Un blob es una lista de trozos, no un buffer contiguo.** Se evita una sola reserva
  gigante y las re-copias al crecer.
- **Cero copias.** Cada frame DATA recibido se guarda tal cual (`zmq::message_t`
  movido con `std::move` a la lista de trozos). Al descargar, se envía con
  `zmq_msg_copy`, que en libzmq comparte el contenido de los mensajes grandes con
  conteo de referencias en vez de copiarlo. *(Verificar con una prueba de memoria
  antes de cerrarlo.)*
- **Propiedad, con la regla acordada:**
  - durante la subida, el blob en construcción tiene un solo dueño:
    `std::unique_ptr<BlobEnConstruccion>`;
  - al confirmar, se mueve a `std::shared_ptr<const Blob>` en el índice. Aquí sí hay
    propiedad compartida real: el índice y cada descarga en curso. Así, borrar o vencer
    una clave nunca libera memoria que alguien está enviando;
  - en la ruta de datos los trozos se mueven, nunca se copian.
- **Presupuesto de memoria:** un tope configurable. Si un PUT declara un tamaño que no
  cabe, el servidor responde de inmediato (ver decisión D1), en vez de fallar a la mitad.
- **Vencimiento:** cada blob tiene TTL. Nunca se descarta en silencio un blob vigente.

## 7. Seguridad e integridad

- CURVE en todas las conexiones; ZAP valida al cliente.
- En HELLO, el job presenta su token OIDC de GitHub; el servidor lo verifica y fija el
  espacio de claves `org/repo/run_id/intento/`. El cliente no puede escribir ni leer
  fuera de su run (ni de otro repositorio).
- BLAKE3 del blob completo en COMMIT y en END. El campo `alg_digest` permite cambiar
  de algoritmo sin romper el protocolo.

## 8. Decisiones abiertas para el dueño

| # | Pregunta | Opciones |
|---|---|---|
| D1 | ¿Qué pasa cuando la memoria no alcanza? | (a) rechazar el PUT con error claro; (b) derramar a disco local (archivo mapeado en memoria); (c) ambas, según configuración |
| D2 | ¿Qué pasa si el servidor se reinicia? | (a) se pierde lo que estaba en memoria y el workflow se reintenta; (b) diario en disco para recuperar |
| D3 | ¿Un GET puede empezar antes del COMMIT (productor y consumidor simultáneos)? | (a) no: NOT_READY o espera con tiempo límite; (b) sí: transmisión en vivo |
| D4 | Tamaño de trozo por defecto | 1, 4 u 8 MiB |
| D5 | Hash por trozo además del hash total | Sí / no |
| D6 | Autenticación | OIDC de GitHub (recomendado) o claves CURVE por runner |
| D7 | Codificador de metadatos | Escrito a mano (recomendado: es pequeño) o generado (estilo zproto) |

## Fuentes

- Protocol Buffers, codificación: https://protobuf.dev/programming-guides/encoding/
- Protocol Buffers, técnicas (mensajes grandes): https://protobuf.dev/programming-guides/techniques/
- 37/ZMTP 3.1: https://rfc.zeromq.org/spec/37/
- 25/ZMTP-CURVE: https://rfc.zeromq.org/spec/25/
- 19/FILEMQ: https://rfc.zeromq.org/spec/19/ — 35/FILEMQ: https://rfc.zeromq.org/spec/35/
- zguide, capítulo 7: https://zguide.zeromq.org/docs/chapter7/
