# JAI - An ultra lightweight jail for AI CLIs on linux

`jai` strives to be the easiest container in the world to
configure--so easy that you never again need to run a code assistant
without protection.  It's not a substitute for
[docker](https://www.docker.com/) or [podman](https://podman.io/) when
you need strong containment.  But if you regularly do risky things
like run an AI CLI with your own privileges in your home directory on
a computer that you care about, then `jai` could reduce the damage
when things go wrong.

`jai` *command* runs *command* with the following policy:

* *command* has complete access to the current working directory.

* *command* has copy-on-write access to the rest of your home
  directory.  It can write there to store dot files, but the changes
  will be kept separate in a `changes` directory and will not actually
  modify your real home directory.

* */tmp* and */var/tmp* are private.

* The rest of the file system is read only.

See the [man page](jai.1.md) for more documentation.

See the [INSTALL](INSTALL) file for installation instructions.
