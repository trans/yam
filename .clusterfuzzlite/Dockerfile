FROM gcr.io/oss-fuzz-base/base-builder:v1
RUN apt-get update && apt-get install -y zip
COPY . $SRC/yam
WORKDIR $SRC/yam
COPY ./.clusterfuzzlite/build.sh $SRC/
