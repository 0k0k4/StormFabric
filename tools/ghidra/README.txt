Dump.java : script Ghidra (headless) utilise pour extraire pseudo-code, vtables, signatures et RTTI
du projet C:\GhidraProjects\Stormworks_decomp (programme stormworks64.exe).

  analyzeHeadless <dossier_projet> Stormworks -process stormworks64.exe -noanalysis -readOnly \
      -scriptPath <dossier de Dump.java> -postScript Dump.java commandes.txt sortie.txt

Commandes (une par ligne dans commandes.txt) :
  D <addr>        decompile la fonction contenant addr
  A <addr> [n]    desassemblage (n instructions max)
  B <addr> <len>  octets
  Q <addr> <n>    n qwords (vtable) avec fonction cible
  X <addr>        references vers addr
  N <texte>       recherche de symboles
  H <vftable>     chaine de classes RTTI d'une vtable
  HA <prefixe>    chaines RTTI de toutes les classes dont le nom commence par <prefixe>
  SIG <addr> <n>  signature d'octets (jokers sur appels/RIP-relatif) + nombre d'occurrences
  F <ascii>       recherche d'une chaine
  G <texte> / GE <suffixe>  recherche dans les instructions
