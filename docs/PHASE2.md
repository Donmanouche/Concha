# Phase 2 — scheduler, synchro, sortie MIDI (16 novembre 2026)

Objectif ([PLAN.md](PLAN.md)) : brancher le moteur de patterns de la phase 1 dans le plugin, avec une synchro au sample près sur le transport du DAW, la sortie MIDI des `.midi()` et un éditeur minimal.

## Ce qui existe maintenant

| Pièce | Fichier | Rôle |
|---|---|---|
| `.midi()` | `js/src/midi.mjs` | Reprend les règles de `@strudel/midi` : `note`, `velocity`×`gain` (0,9 par défaut), `ccn`/`ccv`, `progNum`, `midibend` (14 bits), `miditouch`, `nrpnn`/`nrpv`, `sysexid`/`sysexdata`, `midicmd` (`progNum`, `cc`, `sysex`), `midichan`, `isController`, `defaultmidimap` et `midimaps` (objets). Les messages sortent par la sortie MIDI du plugin, vers le DAW. Différences voulues : un seul port, celui du DAW, donc `midiport` est ignoré ; pas de `noteOffsetMs` (voir ci-dessous) ; `clock`/`start`/`stop` ignorés, puisque c'est le DAW l'horloge. Les `midimaps` données par URL arriveront avec le réseau (phase 3). |
| Fil pattern | `src/sched/PatternWorker` | Deux moteurs JS alternent. L'**actif** répond aux requêtes. L'**autre** évalue le nouveau code sur son propre fil, puis les deux s'échangent. **La musique ne s'arrête pas pendant une évaluation lente**, et en cas d'erreur l'ancien code continue de jouer, comme dans Strudel. Le fil demande à Strudel 0,25 s d'avance, coupe ses requêtes à la fin de la boucle du DAW et reprend au début de la boucle pour le tour suivant. Le nouveau code démarre 100 ms après la tête de lecture. |
| Fil audio | `src/sched/AudioScheduler` | Place chaque événement sur son sample, d'après la position du DAW en noires. **Un changement de tempo ne casse rien.** Un bloc qui traverse la fin de boucle est coupé en deux, et les deux façons de boucler des hôtes sont acceptées. Un saut (relocalisation) jette les événements en attente et relâche les notes tenues. Au même sample, le note-off passe avant le note-on. Les événements de l'ancien code sont jetés à partir du point de bascule, sauf les note-offs. Un retard de moins de 0,03 temps est rattrapé au sample 0, au-delà l'événement est jeté et compté. Ce code temps réel n'alloue pas de mémoire et ne prend aucun verrou. |
| Démarrage préparé | idem | **Transport arrêté**, le fil pattern prépare déjà les événements du départ à la position du curseur. Au lancement, la première note tombe sur le tout premier sample. |
| Pré-écoute | `src/plugin/PreviewSynth` | **Provisoire, en attendant superdough C++ (phase 3)** : les sons avec une hauteur jouent un triangle doux, les autres (`s("bd")`…) un petit bruit coloré selon le nom. Chaque son sort sur la sortie de son orbit. |
| Éditeur | `src/plugin/PluginEditor` | Zone de code : **Ctrl+Entrée** évalue, **Ctrl+.** coupe. Ligne d'état : ok ou erreur, durée de l'évaluation, « modifié », événements, retards, sauts, pire requête. Console avec les logs de Strudel et les erreurs. Le code est sauvegardé avec le projet du DAW et réévalué au chargement. |

Le « sync probe » de la phase 0 reste disponible, désactivé par défaut.

## Tests

`ctest` : 2 suites, **27 tests**, stables (8 exécutions sur 8 sans échec).

- `tests/sched_tests.cpp`, scheduler seul : sample exact, changement de tempo, bloc qui traverse la fin de boucle, hôte qui boucle en début de bloc, saut, ordre note-off puis note-on, remplacement par le nouveau code, tolérance de retard.
- `tests/sched_tests.cpp`, **chaîne complète Strudel → fil pattern → fil audio** sur un transport simulé :
  - `note("c4 d4 e4 f4").midi()` sur 3 tours d'une boucle de 4 temps, avec des blocs de 500 samples (ce n'est pas un diviseur du temps) : **les 12 note-ons tombent exactement aux samples 0, 24000, 48000…**, sans retard, et sans saut à part le départ.
  - CC, Program Change et canal 2 à 90 BPM, sur des tiers de cycle exacts.
  - Évaluation de 300 ms pendant la lecture : l'ancien code continue, puis le nouveau prend le relais, sans retard.
  - Départ depuis un curseur arrêté au temps 9 : la première note sort au sample 0.
- La parité de la phase 1 est inchangée : 0 différence inexpliquée dans QuickJS.

## Le test dans Bitwig (à faire par toi)

À copier d'abord dans `~/.clap` et `~/.vst3`. Le code par défaut joue une batterie et une basse en pré-écoute, sur Main et Orbit 1.

1. Lancer le transport : le son démarre sur le premier temps, et **Ctrl+Entrée** après une modification change le son environ 100 ms plus tard.
2. Mettre une boucle de 2 mesures, déplacer le curseur, changer le tempo pendant la lecture : tout reste sur la grille.
3. Évaluer `$: note("c4 e4 g4 b4").ccn(74).ccv(sine.segment(8)).progNum("<0 1>").midi()`, envoyer la sortie MIDI de Concha vers un synthé, puis enregistrer un clip : notes, CC 74 et Program Change aux bons ticks.
4. Faire une erreur exprès (`s("bd"`) : la console l'affiche et la musique continue.
5. Sauvegarder, fermer, rouvrir le projet : le code revient et rejoue.
6. Pendant la lecture, évaluer quelque chose de lourd, par exemple le morceau `caverave` des exemples de Strudel : pas de trou.

## Choix et limites à connaître

- **Pas de `noteOffsetMs`** : dans Strudel, il raccourcit les notes d'1 ms pour éviter qu'un note-off arrive après le note-on suivant à cause des décalages d'horloge. Ici tout est au sample près, et le scheduler envoie toujours le note-off avant le note-on au même sample.
- **Latence d'un changement de code : 100 ms.** C'est le temps laissé à une requête lourde pour que les premières notes du nouveau code ne soient pas perdues.
- **Après un saut pendant la lecture**, les événements des premières millisecondes peuvent manquer, le temps que le fil pattern recalcule (il ne peut pas deviner où le curseur va sauter). Les boucles du DAW, elles, sont continues.
- Les fils pattern utilisent la pile par défaut du système (8 Mo sous Linux) et QuickJS est limité à 6 Mo. **Pour Windows, où la pile par défaut fait 1 Mo**, il faudra créer ces fils avec une taille de pile explicite.
- Entrée MIDI vers les patterns : phase 7. Les sorties `osc`, `csound`… gardent leur propre genre d'événement et ne sont pas encore jouées.
