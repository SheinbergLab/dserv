# dserv

## Git Submodules

This repo uses [git submodules](https://git-scm.com/book/en/v2/Git-Tools-Submodules) to obtain source code for several dependencies (and specific commits/versions of each):
 - [tcl](https://github.com/tcltk/tcl) 9
 - [jansson](https://github.com/akheron/jansson) JSON
 - [libharu](https://github.com/libharu/libharu) PDF
 - [FLTK](https://github.com/fltk/fltk) GUI framework

To clone this repo along with the submodules:

``` 
git clone --recurse-submodules https://github.com/SheinbergLab/dserv.git
```

If you already have this repo and you want to clone or update the submodules:

```
git pull --recurse-submodules
```

## Releases with GitHub Actions

This repo uses GitHub Actions to build and test dserv whenever we push changes to this repo.
It also uses Actions to build and release dserv whenever we push a repository tag.
The workflow might go like this:

 - Make changes locally.
 - Commit and push changes to this repo.
 - Follow build and test progress at the repo [Actions tab](https://github.com/benjamin-heasly/dserv/actions).
 - Confirm tests are passing.
 - Create a new tag locally.
   - The tag name should be a version number, like `0.0.1`.
   - `git tag --list`
   - Pick a new version number / tag name that doesn't exist yet.
   - Annotated tags with `-a` can have metadata, including messages, which is nice.
   - `git tag -a 0.0.2 -m "Now with lasers."`
   - `git push --tags`
 - When the new tag is pused to GitHub, GitHub Actions will kick off a new build and release.
 - See workflow definition(s) like [package_and_release.yml](./.github/workflows/package_and_release.yml).
 - See completed releases and build artifacts at the repo [Releases](https://github.com/benjamin-heasly/dserv/releases).
