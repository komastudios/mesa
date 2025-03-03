use std::num::ParseIntError;

pub type PResult<'a, O> = std::result::Result<(&'a str, O), ParseError<'a>>;

#[derive(Debug)]
pub enum ErrorKind {
    CustomErr(&'static str),
    Expected(&'static str),
    OneOf(&'static str),
    ParseIntError(ParseIntError),
    EndOfFile,
}

#[derive(Debug)]
pub struct ParseError<'a> {
    pub input: &'a str,
    pub reason: ErrorKind,
    pub is_unrecoverable: bool,
}

impl<'a> ParseError<'a> {
    pub fn new(input: &'a str, reason: ErrorKind) -> Self {
        ParseError {
            input,
            reason,
            is_unrecoverable: false,
        }
    }
}

/// A parser takes in input a string, parses something
/// and returns a tuple containing the remaining data and the
/// parsed object (Parser::O)
///
/// Parsers can fail if the input string cannot be parsed into
/// the expected object, when they do, they return a ParseError
/// object describing the failure.
///
/// Parsers can be joined by using the .and construct, p1.and(p2)
/// would crate a parser that calls p1 first, then p2, returning
/// both the parsed objects. If one of the parsers returns an error
/// the parser returns the error.
///
/// Alternatives can be constructed using the .or construct, p1.or(p2)
/// Will try the parser p1 and, if it fails to parse the content,
/// it will parse the same original text with parser p2.
/// Obviously both parsers must parse the same type of object.
///
/// Errors are recoverable (backtrackable) by default, eg. if a parser
/// fails to parse an object a father parser can try to use other parsers.
/// This can be managed with the `is_unrecoverable` option in ParseError.
///
/// Parsers are inherently single functions, so it is possible to only
/// write even basic constructs with `impl FnMut`. Most constructs have
/// been written with structures and explicit trait impls to help compiler
/// performance as composition of impl trait is currently not handled
/// efficiently: https://github.com/rust-lang/rust/issues/137636
pub trait Parser<'a>: Sized {
    type O;

    fn parse(&self, input: &'a str) -> PResult<'a, Self::O>;

    fn or<B: Parser<'a, O = Self::O>>(self, other: B) -> ParseOr<(Self, B)> {
        ParseOr((self, other))
    }

    fn and<B: Parser<'a>>(self, other: B) -> ParseAnd<(Self, B)> {
        ParseAnd((self, other))
    }

    fn and_then<R>(
        self,
        map: impl Fn(Self::O) -> Result<R, ErrorKind>,
    ) -> impl Fn(&'a str) -> PResult<'a, R> {
        move |input| {
            let (remaining, parsed) = self.parse(input)?;
            let parsed = map(parsed).map_err(|e| ParseError::new(input, e))?;
            Ok((remaining, parsed))
        }
    }

    fn map<R>(
        self,
        fun: impl Fn(Self::O) -> R,
    ) -> impl Fn(&'a str) -> PResult<'a, R> {
        move |input| match self.parse(input) {
            Ok((rem, data)) => Ok((rem, fun(data))),
            Err(x) => Err(x),
        }
    }

    fn opt(self) -> impl Fn(&'a str) -> PResult<'a, Option<Self::O>> {
        move |input| match self.parse(input) {
            Ok((rem, data)) => Ok((rem, Some(data))),
            Err(x) if x.is_unrecoverable => Err(x),
            Err(_) => Ok((input, None)),
        }
    }

    fn ws(self) -> impl Fn(&'a str) -> PResult<'a, Self::O> {
        delimited(whitespace, self, whitespace)
    }

    fn mark_unrecoverable(self) -> impl Fn(&'a str) -> PResult<'a, Self::O> {
        move |input| match self.parse(input) {
            Ok(x) => Ok(x),
            Err(mut x) => {
                x.is_unrecoverable = true;
                Err(x)
            }
        }
    }
}

impl<'a, B, T> Parser<'a> for T
where
    T: Fn(&'a str) -> PResult<'a, B>,
{
    type O = B;

    fn parse(&self, input: &'a str) -> PResult<'a, Self::O> {
        self(input)
    }
}

impl<'a> Parser<'a> for () {
    type O = ();

    fn parse(&self, input: &'a str) -> PResult<'a, Self::O> {
        Ok((input, ()))
    }
}

pub trait WithDefaultParser: Sized {
    fn parse<'a>(input: &'a str) -> PResult<'a, Self>;
}

pub struct AndParser<A, B>(A, B);

impl<'a, A, B> Parser<'a> for AndParser<A, B>
where
    A: Parser<'a>,
    B: Parser<'a>,
{
    type O = (A::O, B::O);

    fn parse(&self, input: &'a str) -> PResult<'a, Self::O> {
        let (input, x) = self.0.parse(input)?;
        let (input, y) = self.1.parse(input)?;

        Ok((input, (x, y)))
    }
}

pub fn take_while<'a>(
    f: impl Fn(char) -> bool,
) -> impl Fn(&'a str) -> PResult<'a, &'a str> {
    move |input: &'a str| {
        let captured = input.chars().take_while(|x| f(*x)).count();
        let (captured, rest) = input.split_at(captured);
        Ok((rest, captured))
    }
}

pub fn take_while1<'a>(
    f: impl Fn(char) -> bool,
) -> impl Fn(&'a str) -> PResult<'a, &'a str> {
    let parser = take_while(f);
    move |input: &'a str| match parser.parse(input) {
        Ok((_r, m)) if m.len() == 0 => Err(ParseError::new(
            input,
            ErrorKind::Expected("take_while1 failed"),
        )),
        x => x,
    }
}

pub fn tag<'a>(tag: &'static str) -> impl Fn(&'a str) -> PResult<'a, &'a str> {
    move |input| {
        if !input.starts_with(tag) {
            return Err(ParseError::new(input, ErrorKind::Expected(tag)));
        }
        let (matched, rest) = input.split_at(tag.len());
        Ok((rest, matched))
    }
}

pub fn many_m<'a, P>(
    m: usize,
    p: P,
) -> impl Fn(&'a str) -> PResult<'a, Vec<P::O>>
where
    P: Parser<'a>,
{
    move |input: &'a str| {
        let mut r = Vec::new();
        let mut cur_input = input;
        loop {
            let (rem, x) = match p.parse(cur_input) {
                Ok(x) => x,
                Err(e) if e.is_unrecoverable => return Err(e),
                Err(_) => break,
            };
            r.push(x);
            cur_input = rem;
        }
        if r.len() >= m {
            Ok((cur_input, r))
        } else {
            Err(ParseError::new(
                input,
                ErrorKind::Expected("Not enough items in list"),
            ))
        }
    }
}

pub fn many0<'a, P>(p: P) -> impl Fn(&'a str) -> PResult<'a, Vec<P::O>>
where
    P: Parser<'a>,
{
    many_m(0, p)
}

pub fn many1<'a, P>(p: P) -> impl Fn(&'a str) -> PResult<'a, Vec<P::O>>
where
    P: Parser<'a>,
{
    many_m(1, p)
}

pub fn separated_list_m<'a, P, S>(
    m: usize,
    p: P,
    s: S,
) -> impl Fn(&'a str) -> PResult<'a, Vec<P::O>>
where
    P: Parser<'a>,
    S: Parser<'a>,
{
    move |input: &'a str| {
        let mut r = Vec::new();
        let mut cur_input = input;
        loop {
            let (rem, x) = match p.parse(cur_input) {
                Ok(x) => x,
                Err(e) if e.is_unrecoverable => return Err(e),
                Err(_) => break,
            };
            r.push(x);
            let rem = match s.parse(rem) {
                Ok((rem, _)) => rem,
                Err(e) if e.is_unrecoverable => return Err(e),
                Err(_) => break,
            };
            cur_input = rem;
        }
        if r.len() >= m {
            Ok((cur_input, r))
        } else {
            Err(ParseError::new(
                input,
                ErrorKind::Expected("Not enough items in list"),
            ))
        }
    }
}

pub fn separated_list0<'a, P, S>(
    p: P,
    s: S,
) -> impl Fn(&'a str) -> PResult<'a, Vec<P::O>>
where
    P: Parser<'a>,
    S: Parser<'a>,
{
    separated_list_m(0, p, s)
}

pub fn separated_list1<'a, P, S>(
    p: P,
    s: S,
) -> impl Fn(&'a str) -> PResult<'a, Vec<P::O>>
where
    P: Parser<'a>,
    S: Parser<'a>,
{
    separated_list_m(1, p, s)
}

pub fn one_of<'a>(
    chars: &'static str,
) -> impl Fn(&'a str) -> PResult<'a, &'a str> {
    move |input: &'a str| {
        let Some(first) = input.chars().next() else {
            return Err(ParseError::new(input, ErrorKind::EndOfFile));
        };
        if input.len() == 0 || !chars.contains(first) {
            return Err(ParseError::new(input, ErrorKind::OneOf(chars)));
        }
        Ok((&input[1..], &input[0..1]))
    }
}

pub fn whitespace<'a>(input: &'a str) -> PResult<'a, &'a str> {
    take_while(|c| " \t".contains(c)).parse(input)
}

pub fn line_comment<'a>(
    start: &'static str,
) -> impl Fn(&'a str) -> PResult<'a, &'a str> {
    tag(start)
        .and(take_while(|c| c != '\n'))
        .map(|(_tag, comm)| comm)
}

pub fn parse_int<'a>(
    input: &'a str,
) -> PResult<'a, (bool, (&'a str, &'a str))> {
    let hexp = tag("0x").and(take_while(|c| c.is_digit(16)));
    let octp = tag("0o").and(take_while(|c| c.is_digit(8)));
    let binp = tag("0b").and(take_while(|c| c.is_digit(2)));
    let norm = take_while(char::is_numeric).map(|x| ("", x));

    one_of("+-")
        .opt()
        .map(|x| x != Some("-"))
        .and(hexp.or(octp).or(binp).or(norm))
        .parse(input)
}

pub fn delimited<'a, A, B, C>(
    prefix: A,
    parser: B,
    postfix: C,
) -> impl Fn(&'a str) -> PResult<'a, B::O>
where
    A: Parser<'a>,
    B: Parser<'a>,
    C: Parser<'a>,
{
    prefix.and(parser).and(postfix).map(|((_, b), _)| b)
}

pub struct PrecededParser<A, B> {
    prefix: A,
    parser: B,
    is_unique: bool,
}

impl<'a, A, B> Parser<'a> for PrecededParser<A, B>
where
    A: Parser<'a>,
    B: Parser<'a>,
{
    type O = B::O;

    fn parse(&self, input: &'a str) -> PResult<'a, Self::O> {
        let (input, _parsd) = self.prefix.parse(input)?;
        let (input, data) = match self.parser.parse(input) {
            Ok(x) => x,
            Err(mut x) if self.is_unique => {
                x.is_unrecoverable = true;
                return Err(x);
            }
            Err(x) => return Err(x),
        };
        Ok((input, data))
    }
}

pub fn preceded<'a, A, B>(prefix: A, parser: B) -> PrecededParser<A, B>
where
    A: Parser<'a>,
    B: Parser<'a>,
{
    PrecededParser {
        prefix,
        parser,
        is_unique: false,
    }
}

pub fn preceded_unique<'a, A, B>(prefix: A, parser: B) -> PrecededParser<A, B>
where
    A: Parser<'a>,
    B: Parser<'a>,
{
    // Can mark inner parser unrecoverable since the prefix is UNIQUE
    // we cannot backtrack and search for other solutions
    PrecededParser {
        prefix,
        parser,
        is_unique: true,
    }
}

pub fn terminated<'a, A, B>(
    parser: A,
    postfix: B,
) -> impl Fn(&'a str) -> PResult<'a, A::O>
where
    A: Parser<'a>,
    B: Parser<'a>,
{
    parser.and(postfix).map(|(a, _b)| a)
}

pub trait ParseOrExt<'a>: Sized {
    fn or(self) -> ParseOr<Self>;
}

pub trait ParseAndExt<'a>: Sized {
    fn and(self) -> ParseAnd<Self>;
}

pub struct ParseOr<T: Sized>(T);

macro_rules! impl_parse_or {
    ($x:ident, $($xn:ident),*) => {

        impl<'a, $x $(, $xn)*> Parser<'a> for ParseOr<($x, $($xn),*)> where $x: Parser<'a>, $($xn: Parser<'a, O = $x::O>), * {
            type O = $x::O;

            fn parse(&self, input: &'a str) -> PResult<'a, Self::O> {
                impl_parse_or_inner!{self input, 0, $x $(, $xn)*}
            }
        }
        impl<'a, $x $(, $xn)*> ParseOrExt<'a> for ($x, $($xn),*) where $x: Parser<'a>, $($xn: Parser<'a, O = $x::O>), * {
            fn or(self) -> ParseOr<Self> {
                ParseOr(self)
            }
        }

        impl_parse_or!{$($xn), *}
    };
    ($x:ident) => {};
}
macro_rules! impl_parse_or_inner {
    ($self:tt $input:tt, $i:tt, $fcount:ident, $($count:ident),*) => {
        match $self.0.$i.parse($input) {
            Err(e) if !e.is_unrecoverable => {}
            x => return x,
        }

        impl_parse_or_inner_succ!{$self $input, $i, $($count), *}
    };
    ($self:tt $input:tt, $i:tt, $last:ident) => {
        $self.0.$i.parse($input)
    };
}

macro_rules! impl_parse_or_inner_succ {
    ($self:tt $input:tt, 0, $($rest:ident),*) => { impl_parse_or_inner!($self $input, 1, $($rest),*) };
    ($self:tt $input:tt, 1, $($rest:ident),*) => { impl_parse_or_inner!($self $input, 2, $($rest),*) };
    ($self:tt $input:tt, 2, $($rest:ident),*) => { impl_parse_or_inner!($self $input, 3, $($rest),*) };
    ($self:tt $input:tt, 3, $($rest:ident),*) => { impl_parse_or_inner!($self $input, 4, $($rest),*) };
    ($self:tt $input:tt, 4, $($rest:ident),*) => { impl_parse_or_inner!($self $input, 5, $($rest),*) };
    ($self:tt $input:tt, 5, $($rest:ident),*) => { impl_parse_or_inner!($self $input, 6, $($rest),*) };
    ($self:tt $input:tt, 6, $($rest:ident),*) => { impl_parse_or_inner!($self $input, 7, $($rest),*) };
    ($self:tt $input:tt, 7, $($rest:ident),*) => { impl_parse_or_inner!($self $input, 8, $($rest),*) };
    ($self:tt $input:tt, 8, $($rest:ident),*) => { impl_parse_or_inner!($self $input, 9, $($rest),*) };
}

impl_parse_or! {
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H
}

pub struct ParseAnd<T: Sized>(T);

macro_rules! impl_parse_and {
    ($x:ident $lx:ident, $($xn:ident $lxn:ident),*) => {
        impl<'a, $x $(, $xn)*> Parser<'a> for ParseAnd<($x, $($xn),*)> where $x: Parser<'a>, $($xn: Parser<'a>), * {
            type O = ($x::O, $($xn::O),*);

            fn parse(&self, input: &'a str) -> PResult<'a, Self::O> {
                let ($lx, $($lxn),*) = &self.0;
                let (input, $lx) = $lx.parse(input)?;
                $(
                    let (input, $lxn) = $lxn.parse(input)?;
                )*
                Ok((input, ($lx, $($lxn),*)))
            }
        }
        impl<'a, $x $(, $xn)*> ParseAndExt<'a> for ($x, $($xn),*) where $x: Parser<'a>, $($xn: Parser<'a>), * {
            fn and(self) -> ParseAnd<Self> {
                ParseAnd(self)
            }
        }

        impl_parse_and!($($xn $lxn), *);
    };
    ($x:ident $lx:ident) => {};
}

impl_parse_and! {
    A a,
    B b,
    C c,
    D d,
    E e,
    F f,
    G g,
    H h
}

// 1-tuple cases, useful for edge-case removal in code generation
impl<'a, A> Parser<'a> for ParseAnd<(A,)>
where
    A: Parser<'a>,
{
    type O = (A::O,);

    fn parse(&self, input: &'a str) -> PResult<'a, Self::O> {
        self.0 .0.parse(input).map(|(r, x)| (r, (x,)))
    }
}
impl<'a, A> ParseAndExt<'a> for (A,)
where
    A: Parser<'a>,
{
    fn and(self) -> ParseAnd<Self> {
        ParseAnd(self)
    }
}
impl<'a, A> Parser<'a> for ParseOr<(A,)>
where
    A: Parser<'a>,
{
    type O = (A::O,);

    fn parse(&self, input: &'a str) -> PResult<'a, Self::O> {
        self.0 .0.parse(input).map(|(r, x)| (r, (x,)))
    }
}
impl<'a, A> ParseOrExt<'a> for (A,)
where
    A: Parser<'a>,
{
    fn or(self) -> ParseOr<Self> {
        ParseOr(self)
    }
}

macro_rules! impl_parse_for_unum {
    ( $( $name:ident ),+ ) => {
        $(impl WithDefaultParser for $name {
            fn parse<'a>(input: &'a str) -> PResult<'a, Self> {
                parse_int.and_then(|(s, (p, n))| {
                    if (!s) {
                        return Err(ErrorKind::Expected("Unsigned integer"));
                    }

                    match p {
                        "0x" => $name::from_str_radix(n, 16),
                        "0o" => $name::from_str_radix(n, 8),
                        "0b" => $name::from_str_radix(n, 2),
                        "" => n.parse(),
                        _ => panic!("Unknown int prefix"),
                    }.map_err(|e| ErrorKind::ParseIntError(e))
                }).parse(input)
            }
        })*
    }
}

macro_rules! impl_parse_for_inum {
    ( $( $name:ident ),+ ) => {
        $(impl WithDefaultParser for $name {
            fn parse<'a>(input: &'a str) -> PResult<'a, Self> {
                parse_int.and_then(|(s, (p, n))| {
                    let n = match p {
                        "0x" => $name::from_str_radix(n, 16),
                        "0o" => $name::from_str_radix(n, 8),
                        "0b" => $name::from_str_radix(n, 2),
                        "" => n.parse(),
                        _ => panic!("Unknown int prefix"),
                    }.map_err(|e| ErrorKind::ParseIntError(e))?;

                    Ok(if s { n } else { -n })
                }).parse(input)
            }
        })*
    }
}

impl_parse_for_unum!(u8, u16, u32, usize);
impl_parse_for_inum!(i8, i16, i32, isize);

pub struct Permutation<T>(pub T);

macro_rules! impl_permutation {
    ($id1:tt, $name1:ident, $type1:ident; $($idn:tt, $namen:ident, $typen:ident); +) => {
        impl<'a, $type1: Parser<'a>, $($typen: Parser<'a>), *> Parser<'a> for Permutation<($type1, $($typen), *)> {
            type O = ($type1::O, $($typen::O), *);

            fn parse(&self, mut input: &'a str) -> PResult<'a, Self::O> {
                let mut res = (Option::<$type1::O>::None, $(Option::<$typen::O>::None), *);
                loop {
                    impl_permutation_loop_inner!(res, self, input, $id1, $($idn),*);

                    break match res {
                        (Some($name1), $(Some($namen)), *) => Ok((input, ($name1, $($namen), *))),
                        _ => unreachable!(),
                    }
                }
            }
        }

        impl_permutation!($($idn, $namen, $typen); *);
    };
    ($id1: tt, $name1:ident, $type1:ident) => {};
}

macro_rules! impl_permutation_loop_inner {
    ($res:tt, $self:tt, $input:tt, $it:tt $(, $tail:tt)*) => {
        impl_permutation_loop_inner!($res, $self, $input, $($tail), *);
        if $res.$it.is_none() {
            match $self.0.$it.parse(&$input) {
              Ok((rest, parsed)) => {
                $input = rest;
                $res.$it = Some(parsed);
                continue;
              }
              Err(x) if x.is_unrecoverable => return Err(x),
              Err(_) => {}
            };
        }
    };
    ($res:tt, $self:tt, $input:tt, ) => {};
}

impl_permutation!(
    7, h, H;
    6, g, G;
    5, f, F;
    4, e, E;
    3, d, D;
    2, c, C;
    1, b, B;
    0, a, A
);

pub struct OptionalPermutation<T>(pub T);

macro_rules! impl_permutation_opt {
    ($id1:tt, $name1:ident, $type1:ident; $($idn:tt, $namen:ident, $typen:ident); +) => {
        impl<'a, $type1: Parser<'a>, $($typen: Parser<'a>), *> Parser<'a> for OptionalPermutation<($type1, $($typen), *)> {
            type O = (Option::<$type1::O>, $(Option::<$typen::O>), *);

            fn parse(&self, mut input: &'a str) -> PResult<'a, Self::O> {
                let mut res = (Option::<$type1::O>::None, $(Option::<$typen::O>::None), *);
                loop {
                    impl_permutation_loop_inner!(res, self, input, $id1, $($idn),*);

                    break Ok((input, res))
                }
            }
        }

        impl_permutation_opt!($($idn, $namen, $typen); *);
    };
    ($id1: tt, $name1:ident, $type1:ident) => {};
}
impl_permutation_opt!(
    7, h, H;
    6, g, G;
    5, f, F;
    4, e, E;
    3, d, D;
    2, c, C;
    1, b, B;
    0, a, A
);
