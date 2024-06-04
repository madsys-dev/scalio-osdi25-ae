FROM golang:1.21.9

ENV HOME=/kv_etcd
ENV GOPATH=/kv_etcd
ENV GOBIN=/kv_etcd/bin
WORKDIR /kv_etcd/src

COPY main.go .
RUN mkdir /kv_etcd/bin
RUN mkdir /kv_etcd/pkg
RUN go env -w GO111MODULE=off
RUN go get .
RUN go build -buildmode=c-shared -o kv_etcd
