# Moniteur témoin — test d'import natif

**Prototype de diagnostic, pas le mod Custom Display.** Import, placement et
vidéo native confirmés par l'utilisateur. XML et archive compagnon de sauvegarde
vérifiés en lecture seule ; le redémarrage complet n'est pas prouvé par ces fichiers.
Il ajoute une identité distincte, `swml_cd_probe_v1_1x1`, portant le nom
**SWML Display Probe 1x1**. Géométrie, collisions, connexions et résolution restent
celles du moniteur natif 1x1 (32×32). Il n'y a pas encore de dialogue de réglage.

## Contenu et limites

- `swml_display_probe/mod.xml` : manifeste du mod de composants natif.
- `swml_display_probe/data/components/<empreinte>.bin` : archive générée à partir
  de votre définition locale. Le nom d'affichage est le seul changement au XML.
- `probe_manifest.json` et `SHA256SUMS.txt` : identités et empreintes de contrôle.

Les meshes sont référencés depuis l'installation existante, sans les copier.
Le générateur préserve le dialecte XML natif, vérifie l'exécutable de référence et
refuse d'écrire dans le jeu ou d'écraser un dossier de sortie. Il n'installe rien,
ne modifie ni les mods existants (dont `customScreen`), ni les véhicules, ni les
profils d'authentification du loader. L'utilisateur a confirmé l'import, le
placement et la vidéo de cette archive. La sauvegarde locale `test.xml` et son
autosave référencent le bon identifiant ; leurs archives compagnon sont identiques
octet pour octet au témoin livré. Cela ne prouve pas à lui seul un redémarrage/rechargement.

Ce dossier a été produit pour un essai local à partir de fichiers de votre jeu ;
il ne constitue pas une archive générique de ressources à redistribuer.

Le témoin passe par le chargement normal des mods natifs au démarrage, pas par
un appel du loader au registre pendant qu'un véhicule le référence. Il ne valide
donc pas encore l'ajout à la volée nécessaire au futur dialogue de configuration.

## Essai manuel isolé (facultatif)

1. Fermer le jeu ; utiliser une sauvegarde et un véhicule de test, en solo.
2. Copier **uniquement** le dossier `swml_display_probe` dans
   `%APPDATA%\Stormworks\data\mods\`, puis l'activer dans la gestion native du jeu.
   Ce n'est **pas** un plugin DLL à mettre dans `Stormworks/mods`.
   Chemin confirmé par l'utilisateur sur cette installation :
   `%APPDATA%\Stormworks\data\mods\swml_display_probe`.
   La présence d'autres dossiers dans `Stormworks/steam_settings/mods/` ne prouvait
   pas que le jeu les chargeait : ce n'est pas le chemin à utiliser pour cet essai.
   Ne pas écraser un dossier préexistant ni modifier `active_mods` manuellement.
3. Redémarrer le jeu si l'activation le demande. Dans l'éditeur de véhicules,
   chercher **SWML Display Probe 1x1**, dans la catégorie des écrans.
4. S'il apparaît, le placer dans un véhicule neuf et l'enregistrer sous un nom
   distinct. Vérifier que le fichier véhicule utilise `swml_cd_probe_v1_1x1`.
5. Quitter entièrement le jeu ; redémarrer, recharger ce véhicule puis le faire
   apparaître en monde. Vérifier les écrans d'origine sur un autre véhicule test.

Un écran noir non connecté ne constitue pas un échec d'import. Le retour utilisateur
suivant confirme une image caméra fonctionnelle. Le tactile, les huit réflexions,
le multijoueur et une résolution personnalisée ne sont toujours pas validés.

**Si le composant n'apparaît pas, ne pas le remplacer par un écran d'origine et
ne pas essayer de forcer un rechargement de registre.** Signaler l'étape exacte,
l'activation native visible et les journaux disponibles. Le journal du loader ne
trace pas nécessairement les rejets du lecteur de composants natif.

En cas de crash : arrêter l'essai, conserver les journaux, désactiver le mod
natif puis retirer seulement ce dossier, jeu fermé. Ne pas ouvrir/enregistrer un
véhicule utilisant ce composant après son retrait : il dépend de sa définition.
Conserver ensemble le véhicule test et les fichiers `.bin` éventuellement créés
par la sauvegarde native. Ne pas supprimer ces données pendant l'analyse.

## Reproduire hors jeu

Depuis le dossier du projet, avec Python 3.11 ou ultérieur :

```text
python tools/custom_display_probe.py build --game ../Stormworks --output build/display-import-probe
python -m unittest discover -s tests -p custom_display_probe_tests.py -v
python tools/custom_display_probe.py verify-save "CHEMIN/vehicule.xml" --reference-archive "CHEMIN/temoin.bin"
```

Le succès des tests et de la relecture par notre outil n'est pas une validation
indépendante du format par Stormworks. Aucun hook ou binding moteur n'a été ajouté.
`verify-save` ne modifie rien et distingue explicitement les preuves disque des
essais runtime ; il ne doit pas être présenté comme un test de redémarrage.
