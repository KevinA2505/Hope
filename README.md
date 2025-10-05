# Simulador de mesas de dominó

## Estado actual del proyecto
- El núcleo está implementado en `domino.c`, que modela partidas simultáneas de dominó con hasta cuatro jugadores por mesa y una cola global de acciones protegida con mutex/condición. El estado de cada mesa conserva el tren de fichas, manos de los jugadores, pozo, política de planificación y sincronización necesaria para coordinar hilos.
- Cada mesa inicia hilos de jugadores productores, un planificador específico de mesa y se integra con un validador único que aplica exactamente una acción por turno antes de despachar al siguiente jugador según la política elegida.
- Hay soporte para cuatro políticas de planificación (FCFS, RR, SJF_POINTS y SJF_PLAYERS) seleccionables en caliente mediante un hilo de control que también permite ajustar el quantum asociado al modo RR o consultar el estado de las mesas.
- El flujo principal pide cuántas mesas crear, inicializa su estado con jugadores aleatorios, lanza todos los hilos auxiliares (validador y consola de control) y espera a que las mesas terminen para liberar recursos.

### Condiciones de finalización
- Una mesa termina inmediatamente cuando un jugador coloca su última ficha (`DOMINA`).
- Si nadie puede jugar y tampoco hay fichas en el pozo, el validador registra pases consecutivos. Cuando la racha de pases (`pass_streak`) alcanza el número de jugadores en la mesa, se cierra la ronda por bloqueo y se declara vencedor al jugador con menor puntaje en la mano.
- Mientras la racha de pases no llegue a todos los jugadores, la mesa continúa, aunque en el log se vea que algunos jugadores ya no tienen jugadas o el pozo esté vacío. Basta con que alguien coloque una ficha válida para reiniciar la racha y mantener la partida en curso.

## Cómo compilar
Compila el ejecutable con GCC y la librería de hilos de POSIX:

```bash
gcc domino.c -lpthread -o domino
```

## Cómo ejecutar
Ejecuta el binario generado y responde al prompt inicial indicando cuántas mesas quieres simular. Durante la ejecución puedes interactuar con la consola de control escribiendo `show`, `policy <mesa|all> <POLÍTICA>` o `quantum <mesa|all> <ms>` para modificar el planificador en caliente.
