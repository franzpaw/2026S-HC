FROM node:24-bookworm-slim

RUN apt-get update \
  && apt-get install -y --no-install-recommends bash ca-certificates git \
  && rm -rf /var/lib/apt/lists/*

RUN npm install -g @earendil-works/pi-coding-agent@0.74.0

WORKDIR /app

COPY package.json package-lock.json tsconfig.json ./
RUN npm ci

COPY src ./src
RUN npm run build \
  && npm prune --omit=dev

ENV NODE_ENV=production
EXPOSE 8010

CMD ["node", "dist/index.js"]
