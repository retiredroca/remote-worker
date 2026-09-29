class RemoteWorker < Formula
  desc "Endpoint agent for the Remote Worker Minecraft mod: control a computer from in-game"
  homepage "https://github.com/retiredroca/remote-worker"
  license "Apache-2.0"

  # version before url, deliberately. The url interpolates #{version}, and a Formula's class body
  # runs top to bottom, so declaring url first would interpolate before the value exists.
  version "1.0.0.26092822"
  url "https://github.com/retiredroca/remote-worker/releases/download/v#{version}/remote-worker-#{version}-macos-arm64"

  # macOS only, and Apple Silicon only.
  #
  # The release workflow builds the agent on ubuntu-latest, windows-latest and macos-14, and
  # macos-14 is arm64. Intel Macs get no build, and saying so here gives a readable error instead of
  # a 404 at download time.
  #
  # This is the same gap the README documents: an Intel macOS agent would need the macos-13 runner
  # added to the matrix in .github/workflows/release-ci.yml, which is a separate decision because
  # those runners are being retired.
  depends_on :macos
  depends_on arch: :arm64

  # Pinned per release. Verified against the published asset, not generated:
  #   shasum -a 256 remote-worker-1.0.0.26092822-macos-arm64
  # Bump with `brew bump-formula-pr` after a release, or edit these two lines by hand.
  sha256 "62c2f65cd1548b9e50996b1869e98615c15d234c9d760f4c1329984dd83ee557"

  def install
    bin.install "remote-worker-#{version}-macos-arm64" => "remote-worker"
  end

  def caveats
    <<~EOS
      This agent is NOT code-signed and NOT notarised, so it has no Apple Developer ID
      signature. Homebrew downloads the file itself, which does not set the quarantine
      attribute, so it should run without a Gatekeeper prompt. If your setup quarantines
      it anyway, the escape hatch is:

          xattr -dr com.apple.quarantine "$(which remote-worker)"

      Then give the machine a key, which prints it once and shows the machine id the
      mod needs:

          remote-worker keygen

      And serve a controller:

          remote-worker agent

      Note that the agent currently has no capture backend. It will authenticate a
      controller and then refuse the session with UnsupportedCapture, because the
      desktop-capture half is not written yet. See the README.
    EOS
  end

  test do
    # --help prints usage and exits 0, and touches no files, so it is safe to run in a test.
    # Asserting on the binary running at all is the point: a broken link or a bad architecture
    # fails here rather than on a user's machine.
    assert_match "usage: remote-worker", shell_output("#{bin}/remote-worker --help 2>&1")
  end
end
