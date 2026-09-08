# StreamVault interview notes

StreamVault has three independent telemetry producers, a UDP recorder, and an offline replay process. It demonstrates process boundaries, sockets, explicit binary layouts, asynchronous timestamps, bounded-memory concurrency, disk persistence, and measurement without becoming a simulator or framework.

## Lifecycle of one packet

The producer wakes on a `steady_clock` schedule, generates a typed payload, assigns a sequence number, and records Unix nanoseconds from `system_clock`. It may intentionally drop the message, but the sequence is already consumed. Otherwise it applies the configured delay and jitter, serializes fields into network byte order, and sends one UDP datagram.

The recorder polls its bound socket so it can notice shutdown without a detached helper. `recvfrom` returns one datagram. The receive thread immediately captures another `system_clock` timestamp, validates and decodes the bytes, updates the source's highest sequence and statistics, and calls `try_push`. The writer wakes through a condition variable, removes records in FIFO order, and appends the length-prefixed representation to disk.

## Networking and serialization

`socket(AF_INET, SOCK_DGRAM, 0)` creates an IPv4 UDP endpoint. Producers use `sendto` with a destination address; the recorder uses `bind`, `poll`, and `recvfrom`. A UDP send is one datagram, so the receiver sees the same message boundary if the datagram arrives. UDP does not promise arrival, order, or uniqueness.

Serialization writes each integer byte explicitly in big-endian order. Doubles are converted to their 64-bit representation with `std::bit_cast` and encoded the same way. Deserialization first proves that lengths are safe, then extracts values, then validates semantic invariants such as source/type pairing and payload count. This order prevents reading outside the supplied buffer.

The session is separate from the UDP format because it adds the recorder timestamp and length-prefix framing. A magic value and version reject the wrong kind of file. Record lengths make truncation and impossible sizes detectable.

## Concurrency and clean shutdown

`BoundedQueue<T>` owns a mutex, condition variable, deque, capacity, closed flag, and high-water mark. `try_push` holds the mutex only while checking and inserting; it never waits for space. `wait_pop` sleeps without busy-waiting until data exists or closure occurs. Once closed, remaining elements can still be popped, then `wait_pop` returns false.

Only the receive thread updates recorder statistics. Only the writer thread touches session output after startup. These ownership rules avoid many races. Queue state is always protected by its mutex. Writer failure is copied under a separate mutex and announced with an atomic flag. Signal handlers only set an atomic stop flag; normal code performs closure and joining.

A clean shutdown stops reception, closes the queue, lets the writer drain it, flushes and closes the session, joins the thread, and then writes the summary. A full queue drops only the persistence copy and increments `queue_drops`; the valid received packet still contributes to receive, gap, and latency statistics.

Possible races include shutdown arriving as a datagram is received, writer failure while reception is polling, and queue closure near a push. The design makes each outcome safe: at most one final datagram is processed, poll checks the writer flag regularly, and `try_push` rejects a closed queue while holding the same mutex used by `close`.

## Measurement and replay

A forward sequence jump estimates missing messages independently per source. Duplicate or old numbers do not move the recorded maximum. This estimates absence but cannot distinguish intentional loss, kernel loss, network loss, or a source failure.

Latency is receive Unix time minus generation Unix time. Mean uses all samples. Minimum and maximum use sorted endpoints. p50 and p99 use nearest rank: `ceil(p * N)`, converted to a zero-based index. Same-host timestamps share a clock, but cross-machine one-way latency needs synchronization and uncertainty bounds.

Replay uses the first record's receive time as zero. For each record it calculates `(record_receive - first_receive) / speed`, maps that duration onto a fresh `steady_clock` start, and calls `sleep_until`. Absolute historical wall time is irrelevant. `max` skips intentional sleeps while preserving file order.

Fault injection uses seeded `std::mt19937_64`. Drop decisions use a uniform real distribution and jitter uses a uniform integer distribution. Determinism depends on the same seed, options, standard-library implementation, and call order. Payload generation has a separate seeded engine so changing fault calls does not perturb sensor values.

## C++ concepts, bottlenecks, and failures

Important C++ features include RAII for sockets and streams, value types for decoded messages, `std::vector` for variable payload storage, `std::span` for non-owning packet input, `std::thread`, `std::mutex`, `std::condition_variable`, atomics, `std::chrono`, `std::filesystem`, exceptions at process boundaries, and `std::bit_cast` for defined floating-point bit conversion.

Likely bottlenecks are one `recvfrom` per datagram, allocation during serialization and decoding, storing every latency sample, console output during replay, and one record write at a time. At much higher rates, socket receive-buffer capacity and scheduler jitter will dominate before the simple calculations do.

Failures are surfaced for invalid CLI input, socket creation, address parsing, bind, poll, receive, send, output open/write/flush/close, malformed packets, corrupt sessions, and nonmonotonic replay order. Limitations include fixed schemas and IDs, no clock synchronization, no CRC, no retransmission, no restart/session identity, and no durability guarantee beyond normal stream flushing.

## 15 likely interview questions

1. **Why use UDP instead of TCP?** UDP preserves datagram boundaries and exposes loss and ordering behavior directly, which is useful for studying telemetry. It trades away reliable, ordered delivery.
2. **Why does the recorder need a writer thread?** Disk I/O can stall. Moving it off the receive path gives the socket loop a better chance of draining incoming datagrams promptly.
3. **Why bound the queue?** An unbounded queue converts sustained overload into uncontrolled memory growth. A bound makes the failure mode explicit and measurable.
4. **What happens when the queue is full?** `try_push` returns false immediately, the receive path continues, and the recorder increments a queue-drop counter.
5. **How are network loss and queue loss distinguished?** Forward sequence gaps estimate messages absent before normal recorder processing; queue drops count valid messages received but not accepted for persistence.
6. **Why keep two timestamps?** Generation time supports latency estimates, while receive time captures the recorder's observed order and replay spacing.
7. **Why use both `steady_clock` and `system_clock`?** `steady_clock` cannot jump and is suited to local scheduling. `system_clock` provides timestamps comparable across same-host processes and storable as Unix time.
8. **How does the condition variable avoid busy-waiting?** `wait` atomically releases the mutex and sleeps, then reacquires it after notification and rechecks a predicate that handles spurious wakeups.
9. **Can sequence gaps measure exact packet loss?** No. They miss loss before the first observation and do not reveal whether absence came from injection, the network, a socket buffer, or producer failure.
10. **How is malformed input handled safely?** The decoder validates minimum and total lengths before field access, caps payload size, and checks semantic invariants before publishing a message.
11. **How is replay drift reduced?** Every deadline is calculated from the original first timestamp and one new steady-clock origin, rather than accumulating many relative sleeps.
12. **What race condition is most important here?** Queue closure racing with insertion. Both operations hold the same mutex, so a record is either inserted before closure or rejected after it.
13. **What does deterministic fault injection guarantee?** With the same implementation, seed, settings, and call order, it repeats drop and jitter decisions. It does not make OS scheduling or UDP delivery deterministic.
14. **What would you change for multiple machines?** Add clock synchronization and clock-quality metadata, instance identities, network security, and probably a more evolvable schema.
15. **What would fail first at very high rates?** Kernel socket buffering, per-message allocations/syscalls, or disk throughput may saturate. Measurements should identify the actual bottleneck before optimizing.
