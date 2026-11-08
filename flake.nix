{
  description = "Nuvasha Launcher - Minecraft launcher for Linux";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
      version = builtins.replaceStrings [ "\n" "\r" " " ] [ "" "" "" ]
        (builtins.readFile ./VERSION);
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        rec {
          nuvashalauncher = pkgs.stdenv.mkDerivation {
            pname = "nuvashalauncher";
            inherit version;

            src = self;

            nativeBuildInputs = with pkgs; [
              gettext
              glib
              pkg-config
              wrapGAppsHook4
            ];

            buildInputs = with pkgs; [
              glib
              gsettings-desktop-schemas
              gtk4
              hicolor-icon-theme
              json-glib
              libadwaita
              libarchive
              libepoxy
              libsecret
              libsoup_3
              shared-mime-info
            ];

            makeFlags = [
              "PREFIX=${placeholder "out"}"
            ];

            enableParallelBuilding = true;

            postInstall = ''
              rm -f "$out/share/glib-2.0/schemas/gschemas.compiled"
              rm -f "$out/share/applications/mimeinfo.cache"
              find "$out/share/mime" -mindepth 1 -maxdepth 1 ! -name packages -exec rm -rf {} +
            '';

            meta = with pkgs.lib; {
              description = "Minecraft launcher for Linux";
              longDescription = ''
                Nuvasha Launcher keeps Minecraft installations separate, so each instance
                can have its own version, loader, Java runtime, settings, mods, and worlds.
                Browse Modrinth to install modpacks, mods, resource packs, and shaders,
                with support for vanilla Minecraft, Fabric, Quilt, Forge, and NeoForge.
              '';
              homepage = "https://github.com/NuvashaLauncher/NuvashaLauncher";
              changelog = "https://github.com/NuvashaLauncher/NuvashaLauncher/releases/tag/v${version}";
              license = with licenses; [ gpl3Only cc0 ];
              mainProgram = "nuvashalauncher";
              platforms = platforms.linux;
            };
          };

          default = nuvashalauncher;
        });

      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/nuvashalauncher";
        };
      });

      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.nuvashalauncher ];
            packages = with pkgs; [
              desktop-file-utils
              gdb
            ];
          };
        });
    };
}
