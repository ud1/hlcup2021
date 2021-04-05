import com.fasterxml.jackson.databind.ObjectMapper;
import io.undertow.Undertow;
import io.undertow.server.HttpHandler;
import io.undertow.server.HttpServerExchange;
import io.undertow.util.Headers;

import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BiConsumer;

public class Main {

    static Seed seed = Seed.S1;
    static boolean limiterEnabled = true;

    static final int SZ = 3500;
    //static final int MAX_POINTS = 1200_000;
    static final int MAX_MS = 30_000;
    static volatile Long endT;

    private static final ScheduledExecutorService scheduler = Executors.newScheduledThreadPool(5);

    static class Cost
    {
        int min, max;

        public Cost(int min, int max) {
            this.min = min;
            this.max = max;
        }
    }
    enum Seed
    {
        S1(Arrays.asList(
                new Cost(3,  5),
                new Cost(7,  11),
                new Cost(12, 18),
                new Cost(15, 23),
                new Cost(17, 27),
                new Cost(18, 30),
                new Cost(20, 32),
                new Cost(21, 35),
                new Cost(22, 36),
                new Cost(23, 37)
        )),
        S2(Arrays.asList(
                new Cost(3,  5),
                new Cost(7,  11),
                new Cost(12, 18),
                new Cost(18, 28),
                new Cost(21, 33),
                new Cost(23, 37),
                new Cost(25, 41),
                new Cost(27, 43),
                new Cost(28, 46),
                new Cost(29, 47)
        )),
        S3(Arrays.asList(
                new Cost(3,  5),
                new Cost(7,  11),
                new Cost(12, 18),
                new Cost(18, 28),
                new Cost(25, 41),
                new Cost(28, 46),
                new Cost(30, 50),
                new Cost(33, 53),
                new Cost(34, 56),
                new Cost(36, 58)
        )),
        S4(Arrays.asList(
                new Cost(3,  5),
                new Cost(7,  11),
                new Cost(12, 18),
                new Cost(18, 28),
                new Cost(25, 41),
                new Cost(34, 56),
                new Cost(37, 61),
                new Cost(39, 65),
                new Cost(42, 68),
                new Cost(43, 71)
        )),
        S5(Arrays.asList(
                new Cost(3,  5),
                new Cost(7,  11),
                new Cost(12, 18),
                new Cost(18, 28),
                new Cost(25, 41),
                new Cost(34, 56),
                new Cost(45, 75),
                new Cost(48, 80),
                new Cost(51, 83),
                new Cost(53, 87)
        ));

        final List<Cost> cost;

        Seed(List<Cost> cost) {
            this.cost = cost;
        }
    }

    static class Req {
        double cost;
        Runnable res;

        public Req(double cost, Runnable res) {
            this.cost = cost;
            this.res = res;
        }
    }

    static class Limiter {
        Set<Req> consumers = new HashSet<>();

        synchronized void request(double cost, Runnable res) throws ApiException {
            if (limiterEnabled)
                consumers.add(new Req(cost, res));
            else
                res.run();
        }

        void addPoints(double points) {
            Set<Req> finished = new HashSet<>();

            synchronized (this)
            {
                if (!consumers.isEmpty()) {


                    while (consumers.size() > 0) {
                        double sub = points / consumers.size();
                        //print("SUB " + sub);
                        boolean oneFinished = false;

                        for (Req r : consumers)
                        {
                            if (r.cost <= sub)
                            {
                                sub = r.cost;
                                finished.add(r);
                                oneFinished = true;
                            }
                        }

                        if (oneFinished)
                        {
                            points -= sub * consumers.size();

                            consumers.removeAll(finished);

                            for (Req r : consumers)
                            {
                                r.cost -= sub;
                            }
                        }
                        else
                        {
                            for (Req r : consumers)
                            {
                                r.cost -= sub;
                            }

                            break;
                        }
                    }
                }
            }

            for (Req r : finished)
            {
                r.res.run();
            }
        }
    }

    static Limiter limiter = new Limiter();

    static class RpsLimiter
    {
        final int MAX_REQS;

        Deque<Long> times = new ArrayDeque<>();

        RpsLimiter(int max_reqs) {
            MAX_REQS = max_reqs;
        }

        synchronized void limit() throws ApiException {
            if (limiterEnabled)
            {
                long t = System.nanoTime();

                long pt = t - 1000_000_000L;
                while (!times.isEmpty() && times.getFirst() < pt)
                {
                    times.removeFirst();
                }

                if (times.size() >= MAX_REQS)
                    throw new ApiException(429, 429, "Too many requests");

                times.addLast(t);
            }
        }
    }

    static RpsLimiter exploreRpsLimiter = new RpsLimiter(1000);
    static RpsLimiter licensesRpsLimiter = new RpsLimiter(350);

    static class Area
    {
        public int posX;
        public int posY;
        public int sizeX;
        public int sizeY;

        @Override
        public String toString() {
            final StringBuffer sb = new StringBuffer("Area{");
            sb.append("posX=").append(posX);
            sb.append(", posY=").append(posY);
            sb.append(", sizeX=").append(sizeX);
            sb.append(", sizeY=").append(sizeY);
            sb.append('}');
            return sb.toString();
        }
    }

    static class Report
    {
        public Area area = new Area();
        public int amount;

        @Override
        public String toString() {
            final StringBuffer sb = new StringBuffer("Report{");
            sb.append("area=").append(area);
            sb.append(", amount=").append(amount);
            sb.append('}');
            return sb.toString();
        }
    }

    static class License
    {
        public int id;
        public int digAllowed;
        public int digUsed;

        @Override
        public String toString() {
            final StringBuffer sb = new StringBuffer("License{");
            sb.append("id=").append(id);
            sb.append(", digAllowed=").append(digAllowed);
            sb.append(", digUsed=").append(digUsed);
            sb.append('}');
            return sb.toString();
        }
    }

    static class Dig
    {
        public int licenseID;
        public int posX;
        public int posY;
        public int depth;

        @Override
        public String toString() {
            final StringBuffer sb = new StringBuffer("Dig{");
            sb.append("licenseID=").append(licenseID);
            sb.append(", posX=").append(posX);
            sb.append(", posY=").append(posY);
            sb.append(", depth=").append(depth);
            sb.append('}');
            return sb.toString();
        }
    }

    static class Treasure {
        String name;
        int price;

        @Override
        public String toString() {
            final StringBuffer sb = new StringBuffer("Treasure{");
            sb.append("name='").append(name).append('\'');
            sb.append(", price=").append(price);
            sb.append('}');
            return sb.toString();
        }
    }

    static int getCost(int d, Random rnd)
    {
        Cost c = seed.cost.get(d - 1);
        return rnd.nextInt(c.max - c.min + 1) + c.min;
    }

    static double getCost(Area a)
    {
        int area = a.sizeX * a.sizeY;

        if (area <= 3)
            return 1;

        double res = 1;
        for (int i = 4; i <= area; i *= 2)
        {
            res += 1;
        }

        return res;
    }

    static double getCost(Dig d)
    {
        return 2.0 * (1.0 + (d.depth - 1) * 0.1);
    }

    static class World
    {
        Map<String, Integer> excavatedTreasures = new ConcurrentHashMap<>();
        Set<String> alreadyExchangedTreasures = new HashSet<>();
        static Set<Integer> givenCoins = new HashSet<>();
        static Map<Integer, License> activeLicenses = new HashMap<>();

        AtomicInteger cashSeq = new AtomicInteger(0);

        int[] heights = new int[SZ*SZ];
        Map<Integer, Map<Integer, Treasure>> treasures = new ConcurrentHashMap<>();
        Map<Integer, Integer> diggedDepth = new ConcurrentHashMap<>();

        public World() {
            for (int i = 0; i < SZ*SZ; ++i)
                heights[i] = 1;

            Random rnd = new Random();

            for (int i = 0; i < 489024; ++i) {
                int x = rnd.nextInt(SZ);
                int y = rnd.nextInt(SZ);
                int d = rnd.nextInt(10) + 1;

                Map<Integer, Treasure> col = treasures.computeIfAbsent(y * SZ + x, v -> new ConcurrentHashMap<>());

                if (!col.containsKey(d)) {
                    Treasure t = new Treasure();
                    t.name = "THR." + i;
                    t.price = getCost(d, rnd);
                    col.put(d, t);
                    treasures.put(0, col);
                }
            }
        }

        void explore(Area area, BiConsumer<Report, Exception> res) {
            try {
                Report report = new Report();
                report.area = area;

                if (area.posX < 0 || area.posY < 0 || area.posX + area.sizeX > SZ || area.posY + area.sizeY > SZ)
                    throw new ApiException(422, 1000, "wrong coordinates: " + area);

                if (area.sizeX <= 0 || area.sizeY <= 0)
                    throw new ApiException(422, 1000, "wrong coordinates: " + area);

                limiter.request(getCost(area), () -> {
                    for (int dy = 0; dy < area.sizeY; ++dy) {
                        for (int dx = 0; dx < area.sizeX; ++dx) {
                            int x = area.posX + dx;
                            int y = area.posY + dy;

                            Map<Integer, Treasure> t = treasures.get(y * SZ + x);
                            if (t != null) {
                                report.amount += t.entrySet().size();
                            }
                        }
                    }

                    res.accept(report, null);
                });
            }
            catch (Exception e)
            {
                res.accept(null, e);
            }
        }


        void dig(Dig dig, BiConsumer<List<String>, Exception> res) throws ApiException {
            try {
                int ind = ind(dig.posX, dig.posY);
                int depth = diggedDepth.getOrDefault(ind, 1);
                if (depth != dig.depth)
                    throw new ApiException(422, 1001, "wrong depth " + dig.depth + " expected " + depth);

                if (dig.posX < 0 || dig.posY < 0 || dig.posX >= SZ || dig.posY >= SZ)
                    throw new ApiException(422, 1000, "wrong coordinates: " + dig);

                limiter.request(getCost(dig), () -> {
                    synchronized (this) {
                        License license = activeLicenses.get(dig.licenseID);
                        if (license == null) {
                            res.accept(null, new ApiException(403, 403, "Invalid license ID " + dig.licenseID));
                            return;
                        }

                        license.digUsed++;
                        if (license.digUsed == license.digAllowed)
                            activeLicenses.remove(dig.licenseID);
                    }

                    diggedDepth.put(ind, depth + 1);
                    Map<Integer, Treasure> col = treasures.get(ind);
                    if (col == null) {
                        res.accept(Collections.emptyList(), null);
                        return;
                    }

                    Treasure tr = col.remove(dig.depth);
                    if (tr == null)
                    {
                        res.accept(Collections.emptyList(), null);
                        return;
                    }

                    excavatedTreasures.put(tr.name, tr.price);
                    res.accept(Collections.singletonList(tr.name), null);
                });
            }
            catch (Exception e)
            {
                res.accept(null, e);
            }
        }

        void cash(String treasure, BiConsumer<List<Integer>, Exception> res) throws ApiException {

            limiter.request(20.0, () -> {
                try {
                    Integer cost = excavatedTreasures.remove(treasure);
                    if (cost == null) {
                        synchronized (this)
                        {
                            if (alreadyExchangedTreasures.contains(treasure))
                                throw new ApiException(409, 1003, "treasure has been already exchanged: " + treasure);
                        }
                        throw new ApiException(409, 1003, "treasure is not digged: " + treasure);
                    }

                    List<Integer> result = new ArrayList<>();

                    for (int i = 0; i < cost; ++i) {
                        result.add(cashSeq.incrementAndGet());
                    }

                    synchronized (this) {
                        givenCoins.addAll(result);
                        alreadyExchangedTreasures.add(treasure);
                    }
                    res.accept(result, null);
                }
                catch (Exception e)
                {
                    res.accept(null, e);
                }
            });
        }

        License newLicense(int[] wallet) throws ApiException {
            License l = new License();
            l.id = licenseSeq.incrementAndGet();
            l.digAllowed = 3;
            l.digUsed = 0;

            if (wallet.length >= 1 && wallet.length <= 5)
                l.digAllowed = 5;
            else if (wallet.length >= 6 && wallet.length <= 10)
                l.digAllowed = 10;
            if (wallet.length >= 11 && wallet.length <= 20)
                l.digAllowed = ThreadLocalRandom.current().nextInt(20, 30);
            if (wallet.length >= 21)
                l.digAllowed = ThreadLocalRandom.current().nextInt(40, 50);

            synchronized (this) {

                if (activeLicenses.size() >= 10)
                    throw new ApiException(409, 1002, "no more active licenses allowed");

                for (int c : wallet) {
                    if (!givenCoins.contains(c))
                        throw new ApiException(403, 403, "Invalid coin " + c);
                }

                for (int c : wallet)
                    givenCoins.remove(c);

                activeLicenses.put(l.id, l);
            }

            return l;
        }

        static int ind(int x, int y)
        {
            return y * SZ + x;
        }
    }

    static AtomicInteger licenseSeq = new AtomicInteger(0);

    static class ApiException extends Exception {
        int httpCode;
        int code;
        String desc;

        public ApiException(int httpCode, int code, String desc) {
            this.httpCode = httpCode;
            this.code = code;
            this.desc = desc;
        }

        @Override
        public String toString() {
            final StringBuffer sb = new StringBuffer("ApiException{");
            sb.append("httpCode=").append(httpCode);
            sb.append(", code=").append(code);
            sb.append(", desc='").append(desc).append('\'');
            sb.append('}');
            return sb.toString();
        }
    }

    static class ErrorResponse {
        public int code;
        public String message;
    }

    static Undertow server;
    static ObjectMapper objectMapper = new ObjectMapper();

    public static void main(String[] args) {
        World w = new World();

        HttpHandler handler = exchange -> {
            if (endT == null)
                endT = System.nanoTime() + MAX_MS * 1000_000L;

            exchange.getRequestReceiver().receiveFullBytes((ex, m) -> {
                try {
                    String body = new String(m, StandardCharsets.UTF_8);

                    String path = exchange.getRelativePath();
                    String method = exchange.getRequestMethod().toString();

                    StringBuilder b = new StringBuilder();
                    b.append(method).append(" ").append(path).append("\n").append(body).append("\n");

                    exchange.getResponseHeaders().put(Headers.CONTENT_TYPE, "application/json");

                    if (path.equals("/explore") && method.equals("POST")) // Report about found treasures.
                    {
                        exploreRpsLimiter.limit();

                        Area area = objectMapper.readValue(body, Area.class);
                        ex.dispatch(() -> {
                            w.explore(area, (report, err) -> {
                                processResult(err, exchange, () -> {
                                    String res = resToString(b, report);
                                    exchange.getResponseSender().send(res);
                                });
                            });
                        });
                    } else if (path.equals("/licenses") && method.equals("POST")) {
                        licensesRpsLimiter.limit();

                        int[] wallet = objectMapper.readValue(body, int[].class);

                        if (limiterEnabled && wallet.length == 0)
                        {
                            if (ThreadLocalRandom.current().nextDouble() <= 0.6)
                            {
                                if (ThreadLocalRandom.current().nextDouble() <= 0.1)
                                {
                                    ex.dispatch(() -> {
                                        scheduler.schedule(() -> {
                                            processError(new ApiException(504, 504, "Error"), exchange);
                                        }, 1000, TimeUnit.MILLISECONDS);
                                    });
                                }
                                else
                                {
                                    ex.dispatch(() -> {
                                        scheduler.schedule(() -> {
                                            processError(new ApiException(502, 502, "Error"), exchange);
                                        }, 10, TimeUnit.MILLISECONDS);
                                    });
                                }

                                return;
                            }
                        }

                        int delayMs = 0;
                        if (limiterEnabled)
                        {
                            delayMs = ThreadLocalRandom.current().nextInt(10, 101);
                        }

                        int finalDelayMs = delayMs;
                        ex.dispatch(() -> {
                            scheduler.schedule(() -> {
                                try {
                                    License l = w.newLicense(wallet);

                                    String res = resToString(b, l);
                                    exchange.getResponseSender().send(res);
                                }
                                catch (Exception e)
                                {
                                    processError(e, exchange);
                                }
                            }, finalDelayMs, TimeUnit.MILLISECONDS);
                        });
                    } else if (path.equals("/dig") && method.equals("POST")) // List of treasures found
                    {
                        Dig dig = objectMapper.readValue(body, Dig.class);
                        ex.dispatch(() -> {
                            try {
                                w.dig(dig, (treasures, err) -> {
                                    processResult(err, exchange, () -> {
                                        if (treasures.isEmpty())
                                            exchange.setStatusCode(404);

                                        String res = resToString(b, treasures);

                                        exchange.getResponseSender().send(res);
                                    });
                                });
                            }
                            catch (Exception e)
                            {
                                processError(e, exchange);
                            }
                        });

                    } else if (path.equals("/cash") && method.equals("POST")) // Payment for treasure
                    {
                        if (ThreadLocalRandom.current().nextDouble() < 0.05)
                            throw new ApiException(503, 503, "Cash error");

                        String treasure = objectMapper.readValue(body, String.class);
                        ex.dispatch(() -> {
                            try {
                                w.cash(treasure, (wallet, err) -> {
                                    processResult(err, exchange, () -> {
                                        String res = resToString(b, wallet);
                                        exchange.getResponseSender().send(res);
                                    });
                                });
                            }
                            catch (Exception e)
                            {
                                processError(e, exchange);
                            }
                        });
                    }
                } catch (Exception e) {
                    processError(e, exchange);
                }
            });
        };

        server = Undertow.builder()
                .addHttpListener(8000, "localhost")
                .setHandler(handler).build();

        if (limiterEnabled)
            new Thread(Main::run).start();
        server.start();
    }

    interface RunnableWithException {
        void run() throws Exception;
    }

    private static String resToString(StringBuilder b, Object obj) throws Exception
    {
        String res = objectMapper.writeValueAsString(obj);
        b.append("Res: ").append(res).append("\n");
        b.append("==============");
        print(b.toString());
        return res;
    }

    private static void processResult(Exception ex, HttpServerExchange exchange, RunnableWithException c)
    {
        try
        {
            if (ex != null)
            {
                throw ex;
            }

            c.run();
        }
        catch (Exception e)
        {
            processError(e, exchange);
        }
    }

    private static void processError(Exception ex, HttpServerExchange exchange)
    {
        try {
            if (ex instanceof ApiException)
            {
                ApiException e = (ApiException) ex;
                exchange.setStatusCode(e.httpCode);
                ErrorResponse err = new ErrorResponse();
                err.code = e.code;
                err.message = e.desc;

                String res = objectMapper.writeValueAsString(err);
                System.err.println(e.toString());
                exchange.getResponseSender().send(res);
            }
            else
            {
                ex.printStackTrace();
            }
        }
        catch (Exception e)
        {
            e.printStackTrace();
        }
    }

    static synchronized void print(String s)
    {
        System.out.println(s);
    }

    static void run()
    {
        long t = System.nanoTime();
        boolean running = true;

        while (running)
        {
            long t2 = System.nanoTime();

            if (endT != null && t2 > endT)
            {
                t2 = endT;
                running = false;
            }

            long dt = t2 - t;
            t = t2;

            double points = 2000.0 * dt / 1.0e9;
            limiter.addPoints(points);

            try {
                Thread.sleep(1);
            } catch (InterruptedException e) {
                e.printStackTrace();
            }
        }

        System.out.println("STOP");
        scheduler.shutdown();
        server.stop();
    }
}
