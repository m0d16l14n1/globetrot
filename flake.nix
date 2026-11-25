{
  description = "Globe/Fn key for Linux: tap to switch keyboard layout, hold for the Fn layer and shortcuts";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: rec {
        globetrot = pkgs.callPackage ./nix/package.nix { };
        default = globetrot;
      });

      overlays.default = final: _prev: {
        globetrot = final.callPackage ./nix/package.nix { };
      };

      nixosModules.default = import ./nix/module.nix self;
      nixosModules.globetrot = self.nixosModules.default;

      checks = forAllSystems (pkgs: {
        package = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      });
    };
}
