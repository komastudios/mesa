// Copyright © 2023 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use proc_macro2::{Span, TokenStream as TokenStream2};
use syn::spanned::Spanned;
use syn::*;

use crate::display_op::{DisplayTokens, ParseTokens};

pub enum RawArg {
    Ident(Ident),
    Literal(LitStr),
    AssignLit(Ident, LitStr),
    AssignType(Ident, Type),
}

impl RawArg {
    pub fn span(&self) -> Span {
        match self {
            RawArg::Literal(x) => x.span(),
            RawArg::Ident(x)
            | RawArg::AssignLit(x, _)
            | RawArg::AssignType(x, _) => x.span(),
        }
    }
}

impl syn::parse::Parse for RawArg {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        if input.peek(syn::LitStr) {
            return Ok(RawArg::Literal(input.parse()?));
        }
        let ident = input.parse()?;
        if !input.peek(Token![=]) {
            return Ok(RawArg::Ident(ident));
        }
        input.parse::<Token![=]>()?;
        let val = if input.peek(syn::LitStr) {
            let rhs = input.parse()?;
            RawArg::AssignLit(ident, rhs)
        } else {
            let rhs = input.parse()?;
            RawArg::AssignType(ident, rhs)
        };
        Ok(val)
    }
}

#[derive(Default, Debug)]
struct OpSourceFormatArgs {
    addr: bool,
    addr_offset: Option<LitStr>,
    custom_format: Option<LitStr>,
    prefix: Option<LitStr>,
}

impl syn::parse::Parse for OpSourceFormatArgs {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        let mut args = OpSourceFormatArgs::default();

        let unhandled_err = |span: Span| {
            return Err(syn::Error::new(span, "Unhandled argument"))
                as syn::Result<()>;
        };

        for arg in
            syn::punctuated::Punctuated::<RawArg, Token![,]>::parse_terminated(
                input,
            )?
            .iter()
        {
            match arg {
                RawArg::Ident(x) if x == "addr" => {
                    args.addr = true;
                }
                RawArg::Literal(fmt) => {
                    args.custom_format
                        .map_or(Ok(()), |_| return unhandled_err(fmt.span()))?;
                    args.custom_format = Some(fmt.clone());
                }
                RawArg::AssignLit(d, offset) if d == "offset" => {
                    args.addr_offset
                        .map_or(Ok(()), |_| return unhandled_err(d.span()))?;
                    args.addr_offset = Some(offset.clone())
                }
                RawArg::AssignLit(d, prefix) if d == "prefix" => {
                    args.prefix
                        .map_or(Ok(()), |_| return unhandled_err(d.span()))?;
                    args.prefix = Some(prefix.clone())
                }
                x => unhandled_err(x.span())?,
            }
        }

        Ok(args)
    }
}

#[derive(Debug)]
pub struct DisplayArgs {
    pub format: LitStr,
}

impl syn::parse::Parse for DisplayArgs {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        let mut format = None;

        for arg in
            syn::punctuated::Punctuated::<RawArg, Token![,]>::parse_terminated(
                input,
            )?
            .iter()
        {
            match arg {
                RawArg::AssignLit(f, fmt) if f == "format" => {
                    format = Some(fmt.clone());
                }
                x => {
                    return Err(syn::Error::new(x.span(), "Unhandled argument"))
                }
            }
        }
        if format == None {
            return Err(input.error("Cannot find format"));
        }

        Ok(DisplayArgs {
            format: format.unwrap(),
        })
    }
}

#[derive(Debug)]
pub enum OpSourceFormat {
    Plain,
    Addr {
        offset: Option<Ident>,
    },
    Custom {
        fmt: LitStr,
        prefix: String,
        postfix: String,
    },
}

fn analyze_custom_format(
    fmt: &str,
) -> std::result::Result<(String, String), &'static str> {
    // Format should be "A{}B"
    // where: A and C can contain {{ or }} (escaped brackets)

    // Equivalent to the regex "[^{]\{\}" compiled by hand
    // (don't want to include the whole re just for this)
    let mut state: u8 = 0u8;
    let mut param_idx = None;
    for (idx, c) in fmt.char_indices() {
        state = match (state, c) {
            (0, '{') => 0,
            (0, _) => 1,
            (1, '{') => 2,
            (1, _) => 1,
            (2, '}') => {
                // found a capture!
                if param_idx.is_some() {
                    return Err("Must only have one parameter print!");
                }
                // '{}' starts at last char, but we are sure it's
                // ASCII (1 byte)
                param_idx = Some(idx - 1);
                1
            }
            (2, '{') => 0,
            (2, _) => 1,
            _ => unreachable!("We only have 3 states"),
        };
    }
    let Some(param_idx) = param_idx else {
        return Err("no parameter print, please add {} in your format str");
    };
    let prefix = fmt[..param_idx].replace("{{", "{").replace("}}", "}");
    let postfix = fmt[(param_idx + 2)..].replace("{{", "{").replace("}}", "}");
    Ok((prefix, postfix))
}

pub fn fn_tuple_to_arr(len: usize) -> TokenStream2 {
    let alphabet = "abcdefghijklmnopqrstuxyvz";
    assert!(alphabet.len() >= len, "too many values to convert");
    let names1 = alphabet[0..len].chars().map(|x| format_ident!("{}", x));
    let names2 = names1.clone();

    quote! {
        |(#(#names1,)*)| [#(#names2,)*]
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SrcDstType {
    Src,
    Dst,
    Label,
}

/// Tracks sources or dests of instructions as Src, Dst and Target
/// Sources can also be arrays of static size
#[derive(Debug)]
pub struct OpSourceDest {
    pub ident: Ident,
    pub ty: SrcDstType,
    /// Src index from the start
    /// for arrays it's the index of the first element
    pub src_type: Option<Ident>,
    pub array_len: usize,
    pub prefix: String,
    pub format: OpSourceFormat,
}

impl OpSourceDest {
    fn parse_field(
        field: &Field,
        match_src: bool,
    ) -> syn::Result<Option<Self>> {
        let (array_len, ty) = match &field.ty {
            Type::Array(TypeArray {
                elem,
                len:
                    Expr::Lit(ExprLit {
                        lit: Lit::Int(len), ..
                    }),
                ..
            }) => (len.base10_parse()?, elem.as_ref()),
            x => (0usize, x),
        };
        let ty = match ty {
            Type::Path(TypePath { qself: None, path }) => path,
            _ => return Ok(None),
        };
        let ty = match ty {
            x if match_src && x.is_ident("Src") => SrcDstType::Src,
            x if match_src && x.is_ident("Label") => SrcDstType::Label,
            x if !match_src && x.is_ident("Dst") => SrcDstType::Dst,
            _ => return Ok(None),
        };

        let attr = field
            .attrs
            .iter()
            .filter(|x| x.path().is_ident("op_format"))
            .next();
        let args = match attr {
            Some(x) => x.parse_args()?,
            None => OpSourceFormatArgs::default(),
        };

        let src_type = if ty == SrcDstType::Src {
            field
                .attrs
                .iter()
                .filter(|x| x.path().is_ident("src_type"))
                .next()
                .map(|x| x.parse_args::<Ident>())
                .transpose()?
        } else {
            None
        };

        let src_prefix =
            args.prefix.map(|x| x.value()).unwrap_or_else(|| "".into());
        let format = if args.addr {
            if !matches!(ty, SrcDstType::Src) {
                return Err(syn::Error::new(
                    field.span(),
                    "Only Src types can have addr formatting!",
                ));
            }
            OpSourceFormat::Addr {
                offset: args
                    .addr_offset
                    .map(|x| Ident::new(&x.value(), x.span())),
            }
        } else if let Some(fmt) = args.custom_format {
            let (prefix, postfix) = analyze_custom_format(&fmt.value())
                .map_err(|x| syn::Error::new(fmt.span(), x))?;
            OpSourceFormat::Custom {
                fmt,
                prefix,
                postfix,
            }
        } else {
            OpSourceFormat::Plain
        };

        Ok(Some(OpSourceDest {
            ident: field.ident.as_ref().unwrap().clone(),
            ty,
            src_type,
            array_len,
            prefix: src_prefix,
            format,
        }))
    }

    fn parse_all(data: &DataStruct, match_src: bool) -> syn::Result<Vec<Self>> {
        data.fields
            .iter()
            .filter_map(|x| Self::parse_field(x, match_src).transpose())
            .collect()
    }

    pub fn parse_all_srcs(data: &DataStruct) -> syn::Result<Vec<Self>> {
        Self::parse_all(data, true)
    }

    pub fn parse_all_dsts(data: &DataStruct) -> syn::Result<Vec<Self>> {
        Self::parse_all(data, false)
    }
}

impl quote::ToTokens for DisplayTokens<&OpSourceDest> {
    fn to_tokens(&self, tokens: &mut TokenStream2) {
        let ident = &self.0.ident;
        assert!(self.0.ty != SrcDstType::Dst, "Cannot format Dsts");

        let generate_no_arr = |ident| {
            let arg = match &self.0.format {
                OpSourceFormat::Plain | OpSourceFormat::Custom { .. } => {
                    quote!( #ident )
                }
                OpSourceFormat::Addr { offset: None } => {
                    quote! { FmtAddr { src: #ident, off: 0 } }
                }
                OpSourceFormat::Addr { offset: Some(off) } => {
                    quote! { FmtAddr { src: #ident, off: self.#off}}
                }
            };
            let fstr = match &self.0.format {
                OpSourceFormat::Custom { fmt, .. } => {
                    format!(" {}{}", self.0.prefix, fmt.value())
                }
                _ => format!(" {}{{}}", self.0.prefix),
            };
            quote! {
                write!(f, #fstr, #arg)?;
            }
        };

        let t = match self.0.array_len {
            0 => generate_no_arr(quote! { self.#ident }),
            n => (0..n)
                .map(|i| generate_no_arr(quote! { self.#ident[#i] }))
                .collect(),
        };

        t.to_tokens(tokens);
    }
}

impl quote::ToTokens for ParseTokens<&OpSourceDest> {
    fn to_tokens(&self, tokens: &mut TokenStream2) {
        let src_type = &self.0.src_type;

        let plain_parser = |ty: SrcDstType| match ty {
            SrcDstType::Src => {
                let src_type = src_type.clone().unwrap_or_else(|| {
                    Ident::new("DEFAULT", Span::call_site())
                });
                quote! {
                    Src::parse(SrcType::#src_type)
                }
            }
            SrcDstType::Dst => quote! {
                Dst::parse
            },
            SrcDstType::Label => quote! {
                Label::parse
            },
        };

        let generate_no_arr = || {
            let arg = match (&self.0.ty, &self.0.format) {
                (ty, OpSourceFormat::Plain) => plain_parser(*ty),
                (
                    ty,
                    OpSourceFormat::Custom {
                        prefix, postfix, ..
                    },
                ) => {
                    let plain = plain_parser(*ty);
                    quote! { crate::parser::delimited(
                        tag(#prefix),
                        #plain,
                        tag(#postfix)
                    )}
                }
                (SrcDstType::Src, OpSourceFormat::Addr { .. }) => {
                    quote! { FmtAddr::parse }
                }
                (ty, fmt) => {
                    panic!("Unknown type-format combination! {ty:?} {fmt:?}")
                }
            };
            let prefix = match self.0.prefix.as_str() {
                "" => quote! {
                    crate::parser::whitespace
                },
                prefix => quote! {
                    crate::parser::whitespace.and(crate::parser::tag(#prefix))
                },
            };
            quote! {
                crate::parser::preceded(#prefix, #arg)
            }
        };

        let t = match self.0.array_len {
            0 => generate_no_arr(),
            n => {
                let parsers = (0..n).map(|_| generate_no_arr());
                let map_fn = fn_tuple_to_arr(n);

                quote! {
                    (#(#parsers,)*).and().map(#map_fn)
                }
            }
        };

        t.to_tokens(tokens);
    }
}

pub fn sources_to_destructure_tokens(
    srcs: &[OpSourceDest],
) -> (TokenStream2, TokenStream2) {
    let destructure_tokens = srcs.iter().map(|x| {
        let ident = &x.ident;
        match &x.format {
            OpSourceFormat::Addr { offset: None } => {
                quote! { FmtAddr { src: #ident, ..  } }
            }
            OpSourceFormat::Addr {
                offset: Some(off_field),
            } => quote! { FmtAddr { src: #ident, off: #off_field  } },
            _ => quote! { #ident },
        }
    });
    let list_tokens = srcs.iter().map(|x| {
        let ident = &x.ident;
        match &x.format {
            OpSourceFormat::Addr {
                offset: Some(off_field),
            } => quote! { #ident, #off_field, },
            _ => quote! { #ident, },
        }
    });
    (
        quote! { (#(#destructure_tokens,)*) },
        quote! { #(#list_tokens)* },
    )
}
