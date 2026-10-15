# The caller sets $checkpoint_dir. Generation 1 may already have been retired.
# Require a durable manifest, not merely a loaded table or a snapshot directory.
# Return its path as $manifest for callers that deliberately damage it.
--let RAPID_TEST_CHECKPOINT_DIR=$checkpoint_dir
--perl
use Time::HiRes qw(time sleep);
my $deadline = time() + 60;
my $dir = $ENV{'RAPID_TEST_CHECKPOINT_DIR'};
while (1) {
  my @manifests = grep { -f $_ && -s $_ } glob("$dir/checkpoint-*.manifest");
  if (@manifests) {
    @manifests = sort {
      my ($a_gen) = $a =~ /checkpoint-(\d+)\.manifest$/;
      my ($b_gen) = $b =~ /checkpoint-(\d+)\.manifest$/;
      $b_gen <=> $a_gen;
    } @manifests;
    open(my $out, '>', "$ENV{'MYSQLTEST_VARDIR'}/tmp/native_checkpoint.inc") or die $!;
    print $out "--let \$manifest = $manifests[0]\n";
    close($out) or die $!;
    last;
  }
  die "No committed native checkpoint in $dir" if time() >= $deadline;
  sleep(0.1);
}
EOF
--source $MYSQLTEST_VARDIR/tmp/native_checkpoint.inc
--remove_file $MYSQLTEST_VARDIR/tmp/native_checkpoint.inc
