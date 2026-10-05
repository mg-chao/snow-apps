# Run with `brew ruby` against the unpublished fixture cask. No Keychain changes.
require "cask/cask_loader"
require "cask/artifact/preflight_block"
require "fileutils"
require "shellwords"

cask = Cask::CaskLoader.load("mg-chao/preflight/snow-shot")
stage = cask.staged_path
raise "Refusing to overwrite an existing Caskroom stage" if stage.exist? || stage.symlink?

begin
  stage.mkpath
  FileUtils.cp(ARGV.fetch(0), stage/"prepare-snow-shot-homebrew.sh")
  # Replace only the signing installer with a probe. Execute the real Homebrew
  # preflight and shipped wrapper, but never touch app installations or identities.
  (stage/"install-snow-shot-macos.sh").write(<<~BASH)
    #!/bin/bash
    set -eu
    test "$HOME" = #{Shellwords.escape(Dir.home)}
    test "$1" = --edition
    test "$2" = full
    test "$3" = --dmg
    test "$5" = --prepare-app
    printf '%s\\n' passed > "$(dirname "$0")/home-probe-result"
  BASH
  preflight = cask.artifacts.find { |artifact| artifact.is_a?(Cask::Artifact::PreflightBlock) }
  raise "Missing preflight hook" unless preflight

  preflight.install_phase(command: SystemCommand)
  raise "Preflight did not preserve the user home" unless (stage/"home-probe-result").read.strip == "passed"

  puts "Native Homebrew preflight preserves the persistent user home."
ensure
  FileUtils.remove_entry(stage) if stage.directory?
end
